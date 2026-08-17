/*
    Open1560 - An Open Source Re-Implementation of Midtown Madness 1 Beta
    Copyright (C) 2020 Brick

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

// Pathway B - the programmable (vs_3_0/ps_3_0) world-geometry path. See game/hlsl/world.{vs,ps}.hlsl.
//
// NOT WIRED UP. Nothing calls Init(), so none of this runs - see the note in agiDX9Pipeline::BeginGfx
// for why the work went to fixed function instead, and what it would take to turn this back on.
//
// This is deliberately a *sibling* of the fixed-function path rather than a replacement. Pathway A
// stays the default because RTX Remix reconstructs its scene from fixed-function draw state - a
// draw issued through a vertex shader is opaque to it, since it cannot know what the shader did to
// the position. Pathway B is for looking good natively.

#include "vector7/vector3.h"

#include "dx9probe.h"

class Matrix34;
class agiDX9TexDef;
class agiViewParameters;

struct IDirect3DDevice9;
struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9;
struct IDirect3DVertexDeclaration9;
struct IDirect3DTexture9;

// Per-texture PBR parameters. MM1 ships colour maps only - there is no roughness, metalness or
// normal data anywhere in the original assets - so these are derived from agiTexProp's authored
// per-texture semantic flags (agiworld/texsheet.h), which is real artist intent rather than
// guesswork: RoadFloorCeiling marks road/floor surfaces, DullOrDamaged marks worn ones, AlphaGlow
// and Lightmap mark emissive ones, Transparent marks glass.
struct agiDX9WorldMaterial
{
    f32 Roughness {0.75f};
    f32 Metalness {0.0f};
    f32 Emissive {0.0f};

    // How much of the mesh's baked vertex colour to fold into the indirect term as ambient
    // occlusion / tint. 0 ignores it entirely, 1 uses it fully.
    f32 AoAmount {1.0f};

    // Content the engine marks as not-to-be-lit: agiTexProp::NotLit, Lightmap (baked light already
    // in the texture) and AlphaGlow (additive sprites). These take the unlit shader permutation,
    // which renders albedo * vertex colour - the same result the fixed-function path gives them.
    bool ForceUnlit {};
};

// Everything one MeshWorld() submission needs to hand the shader. Grouped into a struct so the
// call site stays legible next to the fixed-function branch it sits beside.
struct agiDX9WorldDrawInfo
{
    const Matrix34* World {};
    const Matrix34* ViewZFlip {};
    const agiViewParameters* Proj {};

    agiDX9TexDef* Texture {};

    bool Lit {};
    bool StaticLighting {};
};

// --- Clustered point lighting -----------------------------------------------------------------
//
// The per-draw light budget used to be sixteen, and before that four, because the light set was
// uploaded into pixel-shader CONSTANT registers and ps_3_0 has no relative addressing for those
// (only vertex shaders get a0/aL). A constant-register loop therefore has to unroll, which spends
// the 512-slot instruction budget linearly in the light count - and it also forced a per-draw CPU
// cull-and-sort over every live light, paid again for every mesh in the frame.
//
// Both limits go away by putting the lights in a TEXTURE and looping over it. tex2Dlod's coordinate
// is an ordinary float, so indexing by a loop counter is legal in ps_3_0, the loop body is counted
// once rather than per light, and the whole set can be built once per frame instead of once per
// draw. What is left is deciding which lights a given pixel should look at, and that is what the
// cluster grid below is for.
//
// The grid is a WORLD-SPACE toroidal (wrapping) grid, deliberately not a view-space froxel grid.
// View-space clusters are the textbook choice, but this backend renders more than one view per
// frame - the rear-view mirror has its own view matrix and a horizontally flipped projection - and a
// grid keyed on world position is simply correct for all of them with no per-view rebuild. Wrapping
// rather than bounding the grid means it covers the entire city rather than a box around the camera,
// which matters because a lamp four hundred units away still legitimately lights the street it
// stands on. Two cells that alias onto the same bucket are a whole grid-width apart, so a light
// leaking between them is rejected by its own attenuation window before it contributes anything.
struct agiDX9ClusterGrid
{
    // Cells per axis. 32 x 8 x 32 = 8192 buckets. Y is shallower because a city is far wider than
    // it is tall and vertical aliasing at 8 cells is already well past roof height.
    static constexpr u32 DimX = 32;
    static constexpr u32 DimY = 8;
    static constexpr u32 DimZ = 32;
    static constexpr u32 CellCount = DimX * DimY * DimZ;

    // Lights considered per cell. Overflow drops the weakest, because the pool is inserted in
    // descending energy order.
    static constexpr u32 LightsPerCell = 16;

    // Indices per bucket TEXEL. 1 means an R32F texture and one light per iteration; 4 means
    // A32B32G32R32F and four. Selected at runtime by -d3d9cellpack; see agiDX9WorldShader.
    //
    // The bucket LAYOUT is identical either way - LightsPerCell contiguous floats per cell, which is
    // 16 floats however they are grouped into texels - so the grid writer does not care and is not
    // parameterised. Only the texture format, its width in texels, and how many the shader reads per
    // iteration change.
    static constexpr u32 MaxTexelsPerCell = LightsPerCell;

    // Buckets are laid out as a 2D texture rather than one enormous column: 64 cells per row keeps
    // both dimensions well inside the 4096 limit every ps_3_0 part guarantees.
    static constexpr u32 CellsPerRow = 64;
    static constexpr u32 TexHeight = CellCount / CellsPerRow; // 128

    // Widest the bucket texture can get, for anything that needs a compile-time bound.
    static constexpr u32 MaxTexWidth = CellsPerRow * MaxTexelsPerCell; // 1024

    // Simultaneous lights in the frame's pool. The index is stored as a float in the bucket texture,
    // so this is not a packing limit - it is where the per-frame build cost and the light texture
    // stop being free.
    static constexpr u32 MaxLights = 256;
};

class agiDX9WorldShader
{
public:
    // The environment probe cars reflect. Owned here because it is rebuilt from the same sun and
    // sky the constant upload already has in hand. See dx9probe.h.
    agiDX9SkyProbe Probe;

public:
    // Returns false if this device or this machine cannot support the path (no ps_3_0, no
    // d3dcompiler, a shader that failed to compile). The caller then simply stays on Pathway A -
    // every failure here is soft by design.
    bool Init(IDirect3DDevice9* device);
    void Shutdown();

    bool IsValid() const
    {
        return valid_;
    }

    // Rebuilds the frame's light pool and cluster grid. Called once per frame from
    // agiDX9Pipeline::BeginFrame, immediately after agiUpdateGlowLights() has aged the registry.
    //
    // This replaces the per-draw gather-cull-sort entirely, which is where most of the CPU saving
    // comes from: that work used to be repeated for every mesh submitted, and a night street full of
    // traffic submits a great many.
    void UpdateLights(IDirect3DDevice9* device);

    // Diagnostics, for the per-frame census.
    u32 LightCount() const
    {
        return light_count_;
    }

    u32 CellFill() const
    {
        return cell_fill_;
    }

    // Binds shaders + declaration and uploads every constant for this draw. Leaves the output
    // merger states (alpha, blend, cull, depth) alone - those are still fixed-function and are
    // programmed by MeshWorld() exactly as they are for Pathway A.
    void Setup(IDirect3DDevice9* device, const agiDX9WorldDrawInfo& info);

    // Puts the device back on the fixed-function pipeline, so the pretransformed screen-space path
    // (HUD, text, minimap, particles) is unaffected.
    void Unbind(IDirect3DDevice9* device);

private:
    bool valid_ {};

    IDirect3DVertexShader9* vs_ {};
    IDirect3DPixelShader9* ps_lit_ {};
    IDirect3DPixelShader9* ps_unlit_ {};
    IDirect3DVertexDeclaration9* decl_ {};

    // Bound when a draw has no texture of its own, so the shader can sample unconditionally
    // instead of carrying a branch for it.
    IDirect3DTexture9* white_ {};

    // MaxLights x 2, A32B32G32R32F. Row 0 is xyz = world position, w = 1/reach^2; row 1 is
    // rgb = emitted colour. D3DPOOL_MANAGED rather than a dynamic default-pool texture on purpose:
    // a managed lock hands back the system-memory copy and marks it dirty, so there is no GPU stall
    // to schedule around, and - the reason that matters here - a managed resource survives a device
    // reset. agiDX9Context::ResetDevice() still releases nothing before Reset(), so a default-pool
    // resource would turn every alt-tab into a failed reset. See A7 in the design doc.
    IDirect3DTexture9* light_tex_ {};

    // agiDX9ClusterGrid::TexWidth x TexHeight, A32B32G32R32F. Four light indices per texel, -1 for
    // an empty slot; the shader stops at the first empty texel.
    IDirect3DTexture9* cell_tex_ {};

    f32 cell_size_ {};

    // Bucket packing, chosen at Init from -d3d9cellpack. See the note where it is set.

    u32 cell_pack_ {1};

    u32 texels_per_cell_ {agiDX9ClusterGrid::MaxTexelsPerCell};

    u32 cell_tex_width_ {agiDX9ClusterGrid::MaxTexWidth};
    u32 light_count_ {};
    u32 cell_fill_ {};
};

// Resolves the PBR parameters for a texture from its agiTexProp flags. Exposed for testing and for
// the material-override work described in the design doc.
agiDX9WorldMaterial agiDX9ResolveMaterial(agiDX9TexDef* texture, bool vehicle);

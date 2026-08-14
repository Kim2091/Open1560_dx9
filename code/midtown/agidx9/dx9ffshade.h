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

// Per-pixel Blinn-Phong for the world path, in fixed function.
//
// THE CONSTRAINT THIS EXISTS TO WORK AROUND
//
// D3D9's fixed-function transform-and-lighting unit evaluates lighting at VERTICES. There is no
// render state that changes that - SetLight/D3DLIGHT9 can only ever produce Gouraud, so the lit
// colour is computed at three corners and interpolated across the triangle between them. On MM1's
// geometry that is very visible: a building wall is often two triangles, so an entire facade is lit
// from four samples, and a specular highlight either lands exactly on a vertex or does not exist.
//
// Phong shading - interpolate the normal, evaluate lighting per fragment - is therefore not
// expressible through the lighting unit at all. But it IS expressible through the TEXTURE BLENDING
// unit, which runs per fragment, and that is what this does. It is the pre-shader technique:
//
//   * D3DTSS_TCI_CAMERASPACENORMAL makes the hardware generate the interpolated surface normal as
//     texture coordinates for each fragment.
//   * Those coordinates index a normalisation cube map, whose texel at direction d holds
//     normalize(d)*0.5 + 0.5. Sampling it renormalises the interpolated normal - which linear
//     interpolation shortens between vertices - and hands back a unit normal encoded as colour.
//   * D3DTOP_DOTPRODUCT3 dots that against a second biased vector, computing one dot product per
//     fragment. D3D9 defines it as 4*sum((a-0.5)*(b-0.5)), so both operands use the same 0..1
//     encoding and the 4x cancels the two 0.5 scales.
//
// Put the light direction in that second operand and the result is N.L per pixel: Lambert diffuse,
// per fragment. Put the half-vector there instead and the result is N.H, and squaring it repeatedly
// with D3DTOP_MODULATE(CURRENT, CURRENT) raises it to a power - each stage doubles the Blinn
// exponent. That is Blinn-Phong specular, per fragment.
//
// WHY BLINN AND NOT PHONG
//
// Classic Phong needs pow(R.V, n). Fixed-function texgen can generate N and R but never H or V per
// fragment, so R.V is not available as a dot the blender can take - the reflection vector comes out
// as texture coordinates, not as a colour operand. N.H is, because H can be handed in as a constant.
//
// H is constant only if both the light and the viewer are treated as infinitely distant. That is not
// a new approximation being introduced here: D3D9's own fixed-function lighting makes exactly the
// same one whenever D3DRS_LOCALVIEWER is false, which is how this renderer already runs. So the
// specular lobe's shape is unchanged from what the engine already computes - the only thing that
// changes is that it is evaluated per fragment instead of at three corners.
//
// WHAT THIS COSTS
//
// The diffuse term is one extra additive pass over the mesh, and the specular term another. To keep
// that bounded, only the SUN is promoted; agiMeshLighterFill1/Fill2 stay on the fixed-function
// lighting unit in the base pass. A fill light exists to lift shadow and its interpolation error is
// not what anyone is looking at, while the sun is the light that produces every hard terminator and
// every highlight in the frame.
//
// RTX REMIX
//
// Off by default, and it should stay off for Remix. Remix path-traces the scene and discards the
// game's raster shading entirely, so none of this reaches the final image - while the extra additive
// passes are extra draw submissions it has to classify. This path is for playing WITHOUT Remix.

// Deliberately no dx9pipe.h. dx9pipe.h includes THIS header (it holds an agiDX9FFPerPixel by
// value), so including it back would be a cycle - and whichever of the two a translation unit
// reached first, the other would see agiDX9FFPerPixel used before it was defined. Nothing here
// needs the pipeline anyway.
class agiDX9TexDef;
class Matrix34;
struct IDirect3DDevice9;
struct IDirect3DCubeTexture9;
struct IDirect3DTexture9;

// True when -ffperpixel is set. Checked before anything else so the whole path costs one bool
// compare when it is off.
bool agiDX9PerPixelEnabled();

// Blinn exponent, as the number of squaring stages. n squarings give an exponent of 2^n, so 4 -> 16
// and 6 -> 64. Clamped to what the device's blend-stage budget allows.
u32 agiDX9PerPixelSpecularSteps();

// Per-pixel vehicle reflections. Separate switch from the shading, because it is useful on its own:
// it replaces the CPU per-vertex sphere-map UVs with hardware texgen, so the reflection stops being
// sampled at vertices and interpolated across panels.
bool agiDX9PerPixelReflectEnabled();

class agiDX9FFPerPixel
{
public:
    // Builds the normalisation cube map. Returns false - softly, like every other capability here -
    // when the device cannot do DOT3 or has too few blend stages, and the caller stays on the
    // ordinary per-vertex path.
    bool Init(IDirect3DDevice9* device);
    void Shutdown();

    bool IsValid() const
    {
        return normal_cube_ != nullptr;
    }

    // Additive per-fragment sun passes over a mesh that the base pass has already drawn. `world` and
    // `view` are the same matrices the base pass used; the vertex data is the base pass's own, so
    // the geometry Remix sees is unchanged.
    //
    // Returns the number of passes actually submitted, for the census.
    u32 DrawSunPasses(IDirect3DDevice9* device, const void* vertices, i32 vertex_count, const u16* indices,
        i32 index_count, u32 vertex_stride, IDirect3DTexture9* albedo, const Matrix34& view);

    // Programs stage `stage` to sample `sphere_map` at coordinates generated per fragment from the
    // reflection vector. Replaces the CPU per-vertex UV build for vehicle chrome.
    void SetupReflectionStage(IDirect3DDevice9* device, u32 stage, IDirect3DTexture9* sphere_map, const Matrix34& view);

private:
    IDirect3DCubeTexture9* normal_cube_ {};
    u32 max_squarings_ {};
    u32 max_stages_ {};
};

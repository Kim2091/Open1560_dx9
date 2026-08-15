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

define_dummy_symbol(agidx9_dx9ffshade);

#include "dx9ffshade.h"

#include "agiworld/meshlight.h"
#include "vector7/matrix34.h"

#include "dx9context.h"

#include "dx9_windows.h"
#include "mmsettings/settings.h"

#include <algorithm>
#include <cmath>

static mem::cmd_param PARAM_ffperpixel {
    "ffperpixel", "Per-pixel Blinn-Phong for the sun, in fixed function (not RTX Remix compatible)"};

static mem::cmd_param PARAM_ffperpixel_steps {"ffperpixelsteps", "Blinn exponent as squaring steps: 2^n"};

static mem::cmd_param PARAM_ffperpixel_reflect {
    "ffperpixelreflect", "Generate vehicle reflection coordinates per pixel instead of per vertex"};

bool agiDX9PerPixelEnabled()
{
    return mmSettingBool("ffperpixel");
}

u32 agiDX9PerPixelSpecularSteps()
{
    // 4 squarings, i.e. an exponent of 16. Low by modern standards and deliberately so: MM1's
    // surfaces are enormous and flat, and a tight lobe on a flat wall reads as a moving artifact
    // rather than as a highlight - the same objection documented against the per-vertex specular in
    // dx9rsys.cpp. 0 disables the specular pass and leaves per-pixel diffuse only.
    return static_cast<u32>(std::clamp(PARAM_ffperpixel_steps.get_or<i32>(4), 0, 6));
}

bool agiDX9PerPixelReflectEnabled()
{
    return PARAM_ffperpixel_reflect.get_or(false);
}

// Face size for the normalisation cube. 64 is plenty: it is sampled by a direction and returns that
// direction back, so its only job is to undo the shortening that linear interpolation inflicts on a
// normal between two vertices. The error at 64 is far below the 8-bit quantisation of the result.
static constexpr u32 kNormalCubeSize = 64;

// D3D9's DOT3 is defined as 4 * sum((a - 0.5) * (b - 0.5)) over RGB, so both operands share this
// encoding and the factor of 4 cancels the two halvings exactly. Anything outside the unit sphere
// clamps, which is why the cube stores normalised directions rather than raw ones.
static DWORD EncodeDirection(const Vector3& v)
{
    auto channel = [](f32 c) { return static_cast<u32>(std::clamp((c * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f)); };

    return D3DCOLOR_ARGB(255, channel(v.x), channel(v.y), channel(v.z));
}

static DWORD EncodeColor(const Vector3& c)
{
    auto channel = [](f32 v) { return static_cast<u32>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f)); };

    return D3DCOLOR_ARGB(255, channel(c.x), channel(c.y), channel(c.z));
}

static Vector3 NormalizeOr(const Vector3& v, const Vector3& fallback)
{
    f32 mag2 = v.Mag2();
    return (mag2 > 1.0e-12f) ? (v * (1.0f / std::sqrt(mag2))) : fallback;
}

// Direction of the texel at (s, t) on cube face `face`, in the D3D cube convention.
static Vector3 CubeFaceDirection(u32 face, f32 s, f32 t)
{
    switch (face)
    {
        case D3DCUBEMAP_FACE_POSITIVE_X: return {1.0f, -t, -s};
        case D3DCUBEMAP_FACE_NEGATIVE_X: return {-1.0f, -t, s};
        case D3DCUBEMAP_FACE_POSITIVE_Y: return {s, 1.0f, t};
        case D3DCUBEMAP_FACE_NEGATIVE_Y: return {s, -1.0f, -t};
        case D3DCUBEMAP_FACE_POSITIVE_Z: return {s, -t, 1.0f};
        default: return {-s, -t, -1.0f};
    }
}

bool agiDX9FFPerPixel::Init(IDirect3DDevice9* device)
{
    Shutdown();

    if (!device)
        return false;

    D3DCAPS9 caps {};

    if (FAILED(device->GetDeviceCaps(&caps)))
        return false;

    if (!(caps.TextureOpCaps & D3DTEXOPCAPS_DOTPRODUCT3))
    {
        Warningf("DX9 per-pixel: device cannot do D3DTOP_DOTPRODUCT3, staying per-vertex");
        return false;
    }

    // Per-stage constants carry the light direction and the light colour into two different stages
    // of the same pass. There is exactly one D3DRS_TEXTUREFACTOR, and this pass needs two constants,
    // so without this cap the light colour would have to be folded into the vertex stream - a
    // per-draw CPU vertex rebuild, which is the cost this whole backend is organised around avoiding.
    if (!(caps.PrimitiveMiscCaps & D3DPMISCCAPS_PERSTAGECONSTANT))
    {
        Warningf("DX9 per-pixel: device has no per-stage constants, staying per-vertex");
        return false;
    }

    // The diffuse pass needs four stages and two simultaneous textures; the specular pass needs
    // 2 + squarings. Anything less and the path cannot be expressed.
    if ((caps.MaxTextureBlendStages < 4) || (caps.MaxSimultaneousTextures < 2))
    {
        Warningf("DX9 per-pixel: %u blend stages / %u simultaneous textures is not enough, staying per-vertex",
            caps.MaxTextureBlendStages, caps.MaxSimultaneousTextures);
        return false;
    }

    max_stages_ = caps.MaxTextureBlendStages;

    // Two stages of the specular chain are spoken for before any squaring: stage 0 takes the dot
    // product and the last one applies the light colour.
    max_squarings_ = caps.MaxTextureBlendStages - 2;

    if (FAILED(
            device->CreateCubeTexture(kNormalCubeSize, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &normal_cube_, nullptr)))
    {
        Warningf("DX9 per-pixel: CreateCubeTexture failed, staying per-vertex");
        return false;
    }

    for (u32 face = 0; face < 6; ++face)
    {
        D3DLOCKED_RECT locked {};

        if (FAILED(normal_cube_->LockRect(static_cast<D3DCUBEMAP_FACES>(face), 0, &locked, nullptr, 0)))
        {
            Shutdown();
            return false;
        }

        for (u32 y = 0; y < kNormalCubeSize; ++y)
        {
            DWORD* row = reinterpret_cast<DWORD*>(static_cast<u8*>(locked.pBits) + (y * locked.Pitch));

            const f32 t = ((static_cast<f32>(y) + 0.5f) * (2.0f / kNormalCubeSize)) - 1.0f;

            for (u32 x = 0; x < kNormalCubeSize; ++x)
            {
                const f32 s = ((static_cast<f32>(x) + 0.5f) * (2.0f / kNormalCubeSize)) - 1.0f;

                row[x] = EncodeDirection(NormalizeOr(CubeFaceDirection(face, s, t), {0.0f, 0.0f, 1.0f}));
            }
        }

        normal_cube_->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), 0);
    }

    Displayf("DX9 per-pixel Blinn-Phong enabled: %u blend stages, exponent 2^%u", caps.MaxTextureBlendStages,
        std::min(agiDX9PerPixelSpecularSteps(), max_squarings_));

    return true;
}

void agiDX9FFPerPixel::Shutdown()
{
    if (normal_cube_)
    {
        normal_cube_->Release();
        normal_cube_ = nullptr;
    }

    max_squarings_ = 0;
    max_stages_ = 0;
}

// Rotates a world-space direction into the camera space the DEVICE is using.
//
// Not agiViewParameters::View: MeshWorld negates that matrix's Z column before handing it to
// SetTransform(D3DTS_VIEW), and D3DTSS_TCI_CAMERASPACENORMAL generates its normals through whatever
// the device actually holds. Feeding this the un-flipped view would light the scene with a sun
// mirrored through the Z axis - correct-looking at noon and wrong at every other hour.
static Vector3 ToDeviceCameraSpace(const Vector3& world_dir, const Matrix34& device_view)
{
    Vector3 result;
    result.Dot3x3(world_dir, device_view);
    return NormalizeOr(result, {0.0f, 0.0f, 1.0f});
}

u32 agiDX9FFPerPixel::DrawSunPasses(IDirect3DDevice9* device, const void* vertices, i32 vertex_count,
    const u16* indices, i32 index_count, u32 vertex_stride, IDirect3DTexture9* albedo, const Matrix34& view)
{
    if (!normal_cube_ || (vertex_count <= 0) || (index_count <= 0))
        return 0;

    // agiMeshLighterSun points TOWARD the light - see the note in SetupD3D9StaticLights, which
    // negates it for D3D9's own directional lights because those want the travel direction. DOT3
    // wants the direction to the light, so it is used as-is here.
    const Vector3 light = ToDeviceCameraSpace(agiMeshLighterSun, view);

    // Infinite viewer. The camera looks down +Z in this space - MeshWorld's Z-flip is what puts it
    // there, and BuildProjectionMatrix reads clip w straight off view Z - so the direction from a
    // surface to the eye is -Z. This is the same approximation D3D9's own lighting unit makes with
    // D3DRS_LOCALVIEWER false, which is how the base pass is already running.
    const Vector3 half = NormalizeOr(light + Vector3 {0.0f, 0.0f, -1.0f}, light);

    const u32 primitive_count = static_cast<u32>(index_count) / 3;

    u16* raw_indices = const_cast<u16*>(indices);
    void* raw_vertices = const_cast<void*>(vertices);

    // Shared state for both passes. Additive, no depth write, and depth test left as the base pass
    // set it - these passes only ever paint pixels the base pass already owns.
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

    device->SetTexture(0, normal_cube_);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);

    // The interpolated surface normal, delivered as texture coordinates and renormalised by the
    // lookup. This is the entire reason the path is per-fragment.
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACENORMAL);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CONSTANT);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

    u32 passes = 0;

    // --- Diffuse: albedo * vertex colour * sun colour * (N.L) ------------------------------------
    {
        device->SetTextureStageState(0, D3DTSS_CONSTANT, EncodeDirection(light));

        device->SetTexture(1, albedo);
        device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, albedo ? D3DTOP_MODULATE : D3DTOP_SELECTARG1);
        device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
        device->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TEXTURE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);

        device->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(2, D3DTSS_COLORARG1, D3DTA_CURRENT);
        device->SetTextureStageState(2, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(2, D3DTSS_ALPHAARG1, D3DTA_CURRENT);

        device->SetTextureStageState(3, D3DTSS_CONSTANT, EncodeColor(agiMeshLighterSunColor));
        device->SetTextureStageState(3, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(3, D3DTSS_COLORARG1, D3DTA_CURRENT);
        device->SetTextureStageState(3, D3DTSS_COLORARG2, D3DTA_CONSTANT);
        device->SetTextureStageState(3, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(3, D3DTSS_ALPHAARG1, D3DTA_CURRENT);

        device->SetTextureStageState(4, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(4, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, static_cast<UINT>(vertex_count), primitive_count,
            raw_indices, D3DFMT_INDEX16, raw_vertices, vertex_stride);

        ++passes;
    }

    // --- Specular: sun colour * (N.H)^(2^steps) ---------------------------------------------------
    //
    // Each squaring stage doubles the exponent, so the cost of a tighter highlight is linear in
    // stages while the exponent grows geometrically. No albedo here - a specular highlight is the
    // colour of the light, not of the surface, which is what makes chrome and wet asphalt read
    // correctly. Negative dots clamp to zero inside DOT3, so surfaces facing away from the sun
    // contribute nothing and need no separate N.L mask.
    if (const u32 steps = std::min(agiDX9PerPixelSpecularSteps(), max_squarings_); steps > 0)
    {
        device->SetTextureStageState(0, D3DTSS_CONSTANT, EncodeDirection(half));

        device->SetTexture(1, nullptr);

        u32 stage = 1;

        for (u32 i = 0; i < steps; ++i, ++stage)
        {
            device->SetTextureStageState(stage, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(stage, D3DTSS_COLORARG1, D3DTA_CURRENT);
            device->SetTextureStageState(stage, D3DTSS_COLORARG2, D3DTA_CURRENT);
            device->SetTextureStageState(stage, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(stage, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
        }

        device->SetTextureStageState(stage, D3DTSS_CONSTANT, EncodeColor(agiMeshLighterSunColor));
        device->SetTextureStageState(stage, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(stage, D3DTSS_COLORARG1, D3DTA_CURRENT);
        device->SetTextureStageState(stage, D3DTSS_COLORARG2, D3DTA_CONSTANT);
        device->SetTextureStageState(stage, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(stage, D3DTSS_ALPHAARG1, D3DTA_CURRENT);

        ++stage;

        // max_squarings_ is sized so this terminator always has a stage to live in, but the guard
        // stays: SetTextureStageState on an out-of-range stage is undefined, and the chain is only
        // correctly terminated if the disable actually lands.
        if (stage < max_stages_)
        {
            device->SetTextureStageState(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
            device->SetTextureStageState(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }

        device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, static_cast<UINT>(vertex_count), primitive_count,
            raw_indices, D3DFMT_INDEX16, raw_vertices, vertex_stride);

        ++passes;
    }

    // Hand the device back in a shape the next draw can trust. Texgen especially: leaving
    // TCI_CAMERASPACENORMAL on stage 0 would silently replace the UVs of every following textured
    // draw with normals.
    device->SetTexture(0, nullptr);
    device->SetTexture(1, nullptr);

    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    return passes;
}

void agiDX9FFPerPixel::SetupReflectionStage(
    IDirect3DDevice9* device, u32 stage, IDirect3DTexture9* sphere_map, const Matrix34& view)
{
    // Reflection coordinates per fragment.
    //
    // The CPU path this replaces computes the sphere-map UV once per VERTEX and lets the rasteriser
    // interpolate it, so on a low-poly car body the reflection vector is linearly blended across
    // whole panels and the reflection swims. TCI_CAMERASPACEREFLECTIONVECTOR has the hardware derive
    // it from the interpolated normal at every fragment instead, which removes the dependence on
    // vertex density entirely.
    //
    // Two corrections ride in the texture transform. The generated vector is in the device's camera
    // space, while the game's sphere maps are indexed in WORLD space (decoded from SphereMap - see
    // BuildVehicleReflectionVertices); composing the camera-to-world rotation fixes that, and
    // without it the chrome spins with the camera instead of staying put on the bodywork. Then the
    // 0.5 scale and bias turn a unit vector into a 0..1 lookup.
    //
    // This is the orthographic form of the sphere map: the original divides by
    // sqrt(x^2 + y^2 + (z+1)^2), and a texture transform is linear so it cannot. The difference is a
    // mild radial stretch toward the rim, where a sphere map carries almost no detail anyway. Being
    // per-fragment is worth far more than the exactness of the projection.
    // view is orthonormal - a rotation composed with MeshWorld's single-axis Z flip - so its inverse
    // is its transpose, and the transpose is what turns a camera-space vector back into world space.
    //
    // Written out rather than built with a matrix helper, because the two conventions have to line
    // up exactly. The engine is row-vector (c = w * View), so world = camera * View_transpose, i.e.
    // world.x is the camera vector dotted with View's FIRST ROW and world.y with its second. D3D
    // texture transforms are row-vector too (out = in * T), so those rows become T's columns.
    D3DMATRIX transform {};

    transform._11 = view.m0.x * 0.5f;
    transform._21 = view.m0.y * 0.5f;
    transform._31 = view.m0.z * 0.5f;
    transform._41 = 0.5f;

    transform._12 = view.m1.x * 0.5f;
    transform._22 = view.m1.y * 0.5f;
    transform._32 = view.m1.z * 0.5f;
    transform._42 = 0.5f;

    transform._33 = 1.0f;
    transform._44 = 1.0f;

    device->SetTexture(stage, sphere_map);
    device->SetTextureStageState(stage, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
    device->SetTextureStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
    device->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + stage), &transform);

    device->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}

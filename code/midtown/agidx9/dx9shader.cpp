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

#include "dx9shader.h"

#include "agi/light.h"
#include "agi/rsys.h"
#include "agi/viewport.h"
#include "agirend/lighter.h"
#include "agiworld/glowlight.h"
#include "agiworld/meshlight.h"
#include "agiworld/meshset.h"
#include "agiworld/texsheet.h"
#include "mmcityinfo/state.h"
#include "stream/stream.h"
#include "vector7/matrix34.h"

#include "dx9texdef.h"

#include "dx9_windows.h"

// Declarations only - ID3DBlob, D3D_SHADER_MACRO and the D3DCOMPILE_* flags. We never call
// D3DCompile directly (that would create a link-time import on d3dcompiler.lib and make the
// programmable path a hard dependency of the default build); it is reached through
// GetProcAddress, so including this header costs nothing at link time.
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

define_dummy_symbol(agidx9_dx9shader);

// ?OpenFile@@YA?AV?$Ptr@VStream@@@@PBD00HPBD@Z - the same loader agigl uses for its GLSL
// (agigl/glrsys.cpp, LoadShader), so shaders can live in an archive or as loose files.
ARTS_IMPORT extern Ptr<Stream> OpenFile(const char* file, const char* folder, const char* ext, i32 ext_id,
    const char* desc);

static mem::cmd_param PARAM_d3d9_exposure {"d3d9exposure", "Pathway B exposure multiplier"};
static mem::cmd_param PARAM_d3d9_heightfog {"d3d9heightfog", "Pathway B fog height falloff (per world unit)"};
static mem::cmd_param PARAM_d3d9_tonemap {"d3d9tonemap", "Pathway B ACES filmic tonemapping"};
static mem::cmd_param PARAM_d3d9_glowlights {"d3d9glowlights", "Emit real point lights from AlphaGlow billboards"};
static mem::cmd_param PARAM_d3d9_glowpower {"d3d9glowpower", "Brightness of glow-driven point lights"};
static mem::cmd_param PARAM_d3d9_flashpower {"d3d9flashpower", "Brightness of the lightning flash"};
static mem::cmd_param PARAM_d3d9_cellsize {"d3d9cellsize", "Cluster grid cell size, in world units"};
static mem::cmd_param PARAM_d3d9_lightspec {"d3d9lightspec", "Specular response from clustered point lights"};
static mem::cmd_param PARAM_d3d9_sun {"d3d9sun", "Time-of-day and weather driven sun instead of the engine's"};
static mem::cmd_param PARAM_d3d9_reflect {"d3d9reflect", "Strength of environment reflections"};

// Sun direction and colour for the current MMSTATE.TimeOfDay x MMSTATE.Weather.
//
// Azimuth as well as elevation, so the sun crosses the sky over the day instead of pivoting in one
// plane. Values are chosen to read correctly rather than to be astronomically right: the game has no
// latitude, date or compass, so there is nothing to be accurate *to*, and the useful target is that
// each preset be recognisable at a glance and that shadows and specular highlights fall somewhere
// plausible for the hour.
static void agiDX9ComputeSunLight(Vector3& out_dir, Vector3& out_color)
{
    struct SunPreset
    {
        f32 ElevationDeg; // above the horizon; negative is below it
        f32 AzimuthDeg;   // 0 = +Z, increasing toward +X
        f32 R, G, B;      // colour at full strength
    };

    // Night is a moon, not a sun. Giving it a below-horizon sun would be more literal and worse:
    // the directional term would vanish entirely and the whole night rig would collapse onto the
    // ambient hemisphere, flattening exactly the surfaces - car bodies - that the clustered street
    // lighting is there to shape. A dim, cool, high key reads as moonlight and keeps the geometry.
    static const SunPreset kByTime[4] {
        {18.0f, 95.0f, 1.00f, 0.82f, 0.62f},  // Morning - low, warm, from the east
        {74.0f, 195.0f, 1.00f, 0.97f, 0.92f}, // Noon - near overhead, almost white
        {9.0f, 268.0f, 1.00f, 0.55f, 0.30f},  // Sunset - very low, heavily reddened, from the west
        {52.0f, 25.0f, 0.32f, 0.38f, 0.55f},  // Night - moonlight, cool and dim
    };

    // Weather scales the sun's strength and pulls it toward the sky's own colour. Overcast does not
    // merely dim a sunbeam, it converts it into a diffuse source, so the sensible knob here is how
    // much key light survives as a directional term at all - the rest is already accounted for by
    // the hemisphere irradiance above.
    struct WeatherPreset
    {
        f32 Scale;
        f32 Desaturate; // 0 = keep the sun's own hue, 1 = fully neutral
    };

    static const WeatherPreset kByWeather[4] {
        {1.00f, 0.00f}, // Sun
        {0.55f, 0.65f}, // Fog   - washed out and neutral
        {0.38f, 0.75f}, // Rain  - the dimmest, and coldest
        {0.70f, 0.55f}, // Snow  - dim sun, but snow scatters a lot back up
    };

    const i32 time_index = std::clamp(static_cast<i32>(MMSTATE.TimeOfDay), 0, 3);
    const i32 weather_index = std::clamp(static_cast<i32>(MMSTATE.Weather), 0, 3);

    const SunPreset& time = kByTime[time_index];
    const WeatherPreset& weather = kByWeather[weather_index];

    constexpr f32 kDeg = 3.14159265f / 180.0f;

    const f32 elevation = time.ElevationDeg * kDeg;
    const f32 azimuth = time.AzimuthDeg * kDeg;

    // Pointing TOWARD the light, matching what agiMeshLighter* stores and what the shader expects.
    const f32 horizontal = std::cos(elevation);

    out_dir.x = horizontal * std::sin(azimuth);
    out_dir.y = std::sin(elevation);
    out_dir.z = horizontal * std::cos(azimuth);

    const f32 luma = (time.R * 0.299f) + (time.G * 0.587f) + (time.B * 0.114f);
    const f32 desat = weather.Desaturate;

    out_color.x = (time.R + ((luma - time.R) * desat)) * weather.Scale;
    out_color.y = (time.G + ((luma - time.G) * desat)) * weather.Scale;
    out_color.z = (time.B + ((luma - time.B) * desat)) * weather.Scale;
}

// ---------------------------------------------------------------------------------------------
// Shader compilation
//
// D3DCompile is reached through LoadLibrary/GetProcAddress rather than by linking d3dcompiler.lib.
// Two reasons, both about failure behaviour: a machine without the DLL must fall back to Pathway A
// rather than fail to start, and nothing about the default (fixed-function) configuration should
// gain a new hard dependency. Compiling at runtime rather than shipping precompiled bytecode is a
// deliberate development choice - a full rebuild of this project is about seven minutes, so being
// able to edit HLSL and just relaunch is the difference between iterating on shading in an
// afternoon and in a week.
// ---------------------------------------------------------------------------------------------

using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID src_data, SIZE_T src_size, LPCSTR source_name,
    const D3D_SHADER_MACRO* defines, void* include, LPCSTR entrypoint, LPCSTR target, UINT flags1, UINT flags2,
    ID3DBlob** code, ID3DBlob** error_msgs);

static PFN_D3DCompile GetD3DCompile()
{
    static PFN_D3DCompile compile = []() -> PFN_D3DCompile {
        // 47 is the version shipped in-box with Windows since 8.1 and redistributed back to Vista.
        // The older numbered ones are only present if a legacy DX SDK was installed, but they cost
        // nothing to try.
        static const char* kNames[] {"d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll"};

        for (const char* name : kNames)
        {
            if (HMODULE module = LoadLibraryA(name))
            {
                if (auto result = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(module, "D3DCompile")))
                {
                    Displayf("DX9 Pathway B: using %s", name);
                    return result;
                }
            }
        }

        Warningf("DX9 Pathway B: no d3dcompiler DLL found, staying on the fixed-function path");
        return nullptr;
    }();

    return compile;
}

static ConstString LoadShaderSource(const char* name, const char* ext)
{
    Ptr<Stream> input = OpenFile(name, "hlsl", ext, 0, "HLSL Shader");

    if (!input)
        return ConstString {};

    isize size = static_cast<isize>(input->Size());
    ConstString result {static_cast<usize>(size) + 1};

    if (input->Read(result.get(), size) != size)
        return ConstString {};

    result[size] = '\0';
    return result;
}

// Compiles one shader. `lit` selects the LIT permutation of the pixel shader; see world.ps.hlsl.
static ID3DBlob* CompileShader(const char* name, const char* ext, const char* target, bool lit)
{
    PFN_D3DCompile compile = GetD3DCompile();

    if (!compile)
        return nullptr;

    ConstString source = LoadShaderSource(name, ext);

    if (!source)
    {
        Warningf("DX9 Pathway B: could not open shader '%s%s'", name, ext);
        return nullptr;
    }

    const D3D_SHADER_MACRO lit_defines[] {{"LIT", "1"}, {nullptr, nullptr}};

    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;

    // OPTIMIZATION_LEVEL1, deliberately, and this is not a quality compromise - measured, not
    // assumed. On the clustered pixel shader, levels 2 and 3 spend FORTY SECONDS and emit byte-for-
    // byte the same program as level 1:
    //
    //     /O0   1,270 ms   458 instruction slots
    //     /O1   1,495 ms   454 instruction slots
    //     /O2  41,543 ms   454 instruction slots
    //     /O3  41,552 ms   454 instruction slots
    //
    // The ps_3_0 backend's optimiser thrashes on the light loop - real flow control containing
    // tex2Dlod and four inlined light evaluations - and finds nothing. That cost is paid on every
    // load, not once at startup: BeginGfx() runs again when the resolution changes, so entering the
    // city recompiles all three shaders. It was the whole of a large, mysterious increase in
    // loading time.
    //
    // If a future shader genuinely benefits from the higher levels, measure it the same way before
    // raising this - and note the ceiling is the 512-slot limit, which level 1 is already meeting.
    HRESULT hr = compile(source.get(), std::strlen(source.get()), name, lit ? lit_defines : nullptr, nullptr, "main",
        target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);

    if (errors)
    {
        // Warnings arrive here too, on an otherwise successful compile - worth printing either way,
        // since this file is loaded from disk and is expected to be edited.
        Warningf("DX9 Pathway B: %s%s: %s", name, ext, static_cast<const char*>(errors->GetBufferPointer()));
        errors->Release();
    }

    if (FAILED(hr))
    {
        if (code)
            code->Release();

        return nullptr;
    }

    return code;
}

bool agiDX9WorldShader::Init(IDirect3DDevice9* device)
{
    Shutdown();

    D3DCAPS9 caps {};
    device->GetDeviceCaps(&caps);

    // ps_3_0 is the floor: the BRDF loop needs the instruction count, arbitrary swizzles and
    // ddx/ddy-class features that ps_2_0 cannot supply, and VFACE (used for two-sided facades)
    // only exists from 3.0. Anything less quietly stays on Pathway A.
    if (caps.VertexShaderVersion < D3DVS_VERSION(3, 0) || caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
    {
        Warningf("DX9 Pathway B: device reports vs_%u_%u / ps_%u_%u, need 3_0 - staying on the fixed-function path",
            (caps.VertexShaderVersion >> 8) & 0xFF, caps.VertexShaderVersion & 0xFF,
            (caps.PixelShaderVersion >> 8) & 0xFF, caps.PixelShaderVersion & 0xFF);
        return false;
    }

    ID3DBlob* vs_code = CompileShader("world", ".vs.hlsl", "vs_3_0", false);
    ID3DBlob* ps_lit_code = CompileShader("world", ".ps.hlsl", "ps_3_0", true);
    ID3DBlob* ps_unlit_code = CompileShader("world", ".ps.hlsl", "ps_3_0", false);

    bool ok = vs_code && ps_lit_code && ps_unlit_code;

    if (ok)
    {
        ok = SUCCEEDED(device->CreateVertexShader(static_cast<const DWORD*>(vs_code->GetBufferPointer()), &vs_)) &&
            SUCCEEDED(device->CreatePixelShader(static_cast<const DWORD*>(ps_lit_code->GetBufferPointer()), &ps_lit_)) &&
            SUCCEEDED(
                device->CreatePixelShader(static_cast<const DWORD*>(ps_unlit_code->GetBufferPointer()), &ps_unlit_));
    }

    if (vs_code)
        vs_code->Release();

    if (ps_lit_code)
        ps_lit_code->Release();

    if (ps_unlit_code)
        ps_unlit_code->Release();

    if (!ok)
    {
        Shutdown();
        return false;
    }

    // Matches agiWorldVtx (agi/vertex.h) exactly. D3DDECLTYPE_D3DCOLOR is correct for the u32
    // colour: it is stored 0xAARRGGBB, which in little-endian memory is B,G,R,A - precisely the
    // byte order D3DCOLOR describes, and D3D9 swizzles it to RGBA on the way into the shader.
    static const D3DVERTEXELEMENT9 kElements[] {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()};

    if (FAILED(device->CreateVertexDeclaration(kElements, &decl_)))
    {
        Shutdown();
        return false;
    }

    // 1x1 opaque white, bound whenever a draw has no texture. Cheaper than a shader permutation
    // and cheaper than a runtime branch, and it keeps the untextured case on exactly the same code
    // path as everything else.
    if (SUCCEEDED(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &white_, nullptr)))
    {
        D3DLOCKED_RECT locked;

        if (SUCCEEDED(white_->LockRect(0, &locked, nullptr, 0)))
        {
            *static_cast<u32*>(locked.pBits) = 0xFFFFFFFF;
            white_->UnlockRect(0);
        }
    }

    // Clustered lighting storage. Float formats because both textures store light INDICES and
    // positions, which have to come back out of the sampler exactly - a packed 8-bit format would
    // need a sentinel value stolen out of the index range, and a half-float one would need
    // conversion on the CPU for no saving worth having.
    //
    // The bucket texture is single-channel: one light index per texel is what lets the pixel shader
    // evaluate the light body once instead of four times. See agiDX9ClusterGrid::TexelsPerCell for
    // the measurements. R32F holds the same 16 floats per cell that A32B32G32R32F did, in the same
    // 512 KB, so this is a layout change rather than a memory one - which is also why the writer
    // below is unchanged: it always wrote LightsPerCell contiguous floats per cell.
    //
    // A failure here is soft in the usual way, but it takes the whole path down rather than
    // degrading, because the pixel shader has no non-clustered branch to fall back to. On anything
    // that reports ps_3_0 both formats are present.
    const HRESULT light_hr = device->CreateTexture(
        agiDX9ClusterGrid::MaxLights, 2, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &light_tex_, nullptr);

    const HRESULT cell_hr = device->CreateTexture(
        agiDX9ClusterGrid::TexWidth, agiDX9ClusterGrid::TexHeight, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &cell_tex_, nullptr);

    if (FAILED(light_hr) || FAILED(cell_hr))
    {
        Warningf(
            "DX9 Pathway B: no A32B32G32R32F/R32F texture support (0x%X / 0x%X) - staying on the fixed-function path",
            static_cast<u32>(light_hr), static_cast<u32>(cell_hr));
        Shutdown();
        return false;
    }

    cell_size_ = std::max(PARAM_d3d9_cellsize.get_or(24.0f), 1.0f);

    // Soft failure: without a probe the shader falls back to the flat hemisphere term it used
    // before, which is worse-looking but perfectly functional.
    Probe.Init(device);

    valid_ = true;
    Displayf("DX9 Pathway B: programmable path active (vs_3_0/ps_3_0), clustered lighting %ux%ux%u cells, %u lights",
        agiDX9ClusterGrid::DimX, agiDX9ClusterGrid::DimY, agiDX9ClusterGrid::DimZ, agiDX9ClusterGrid::MaxLights);

    return true;
}

void agiDX9WorldShader::Shutdown()
{
    valid_ = false;

    Probe.Shutdown();

    if (vs_)
    {
        vs_->Release();
        vs_ = nullptr;
    }

    if (ps_lit_)
    {
        ps_lit_->Release();
        ps_lit_ = nullptr;
    }

    if (ps_unlit_)
    {
        ps_unlit_->Release();
        ps_unlit_ = nullptr;
    }

    if (decl_)
    {
        decl_->Release();
        decl_ = nullptr;
    }

    if (white_)
    {
        white_->Release();
        white_ = nullptr;
    }

    if (light_tex_)
    {
        light_tex_->Release();
        light_tex_ = nullptr;
    }

    if (cell_tex_)
    {
        cell_tex_->Release();
        cell_tex_ = nullptr;
    }

    light_count_ = 0;
    cell_fill_ = 0;
}

// ---------------------------------------------------------------------------------------------
// Material resolution
// ---------------------------------------------------------------------------------------------

agiDX9WorldMaterial agiDX9ResolveMaterial(agiDX9TexDef* texture, bool vehicle)
{
    agiDX9WorldMaterial mtl {};

    if (vehicle)
    {
        // Car paint is a DIELECTRIC, not a metal. Automotive paint is pigment suspended under a
        // clear lacquer; the glossy highlight comes from the lacquer's smooth dielectric surface,
        // not from a conductive base. Modelling it as metalness 0.55 was wrong twice over:
        //   * `diffuse_color = albedo * (1 - metalness)` threw away 55% of the paint colour, and
        //   * a metal has no diffuse lobe at all, so the missing energy has to come back as an
        //     environment reflection - and this path's environment is a two-lobe hemisphere built
        //     from agiMeshLighterAmbient, which is nowhere near bright enough to stand in for one.
        // The result was a *yellow* Panoz rendering as dark olive-black, which is the classic
        // signature of metalness without an environment to reflect.
        // Low roughness on a dielectric gives the tight lacquer highlight without any of that.
        mtl.Roughness = 0.30f;
        mtl.Metalness = 0.0f;
        mtl.AoAmount = 0.5f;
    }

    if (!texture)
        return mtl;

    const u32 props = texture->Tex.Props;

    if (props & agiTexProp::RoadFloorCeiling)
        mtl.Roughness = 0.88f; // asphalt, concrete, pavement

    if (props & agiTexProp::DullOrDamaged)
        mtl.Roughness = std::max(mtl.Roughness, 0.95f);

    if (props & agiTexProp::Transparent)
    {
        // Glass: smooth, dielectric. Its f0 stays at the 0.04 default, which is right for glass.
        mtl.Roughness = 0.15f;
        mtl.Metalness = 0.0f;
    }

    if (props & agiTexProp::Chromakey)
    {
        // Alpha-keyed foliage, fences, railings. Flat cut-outs with no meaningful microfacet
        // structure - a specular lobe on them reads as a mistake.
        mtl.Roughness = 0.95f;
        mtl.Metalness = 0.0f;
    }

    // Emissive. AlphaGlow is the engine's own marker for additively-composited glow content
    // (coronas, headlight glows, lit signage) and Lightmap for baked light; NotLit marks content
    // that must not receive lighting at all. All three want to survive the tonemap rather than be
    // crushed with the rest of the frame.
    // Emissive/unlit content REPLACES lighting, it does not add to it.
    //
    // This previously set an Emissive value that the shader added on top of the full direct and
    // indirect result (`color += albedo * emissive`). For anything flagged NotLit or Lightmap that
    // is roughly 2x albedo, which saturates to flat white - and NotLit means precisely "do not light
    // this surface", so lighting it and then adding its albedo again is wrong twice over. That is
    // what turned traffic cars and a lot of city meshes white.
    //
    // Routing them through the existing unlit permutation instead is both correct and free: it
    // renders albedo * vertex colour, which is exactly what the fixed-function path does with them.
    if (props & (agiTexProp::AlphaGlow | agiTexProp::Lightmap | agiTexProp::NotLit))
        mtl.ForceUnlit = true;

    return mtl;
}

// ---------------------------------------------------------------------------------------------
// Constant upload
// ---------------------------------------------------------------------------------------------

static void Mul4x4(D3DMATRIX& out, const D3DMATRIX& a, const D3DMATRIX& b)
{
    for (i32 r = 0; r < 4; ++r)
    {
        for (i32 c = 0; c < 4; ++c)
        {
            out.m[r][c] = (a.m[r][0] * b.m[0][c]) + (a.m[r][1] * b.m[1][c]) + (a.m[r][2] * b.m[2][c]) +
                (a.m[r][3] * b.m[3][c]);
        }
    }
}

// D3D9 constant registers are row-vectors read as float4 rows, but HLSL's default matrix packing
// for a float4x4 constant is column-major - so a matrix built for the row-vector `mul(v, M)` form
// has to be transposed on the way in.
static void SetMatrix(IDirect3DDevice9* device, u32 reg, const D3DMATRIX& m)
{
    f32 transposed[16];

    for (i32 r = 0; r < 4; ++r)
        for (i32 c = 0; c < 4; ++c)
            transposed[(r * 4) + c] = m.m[c][r];

    device->SetVertexShaderConstantF(reg, transposed, 4);
}

// Inverse transpose of the world matrix' 3x3 part, for normals under scaled instances. Uploaded as
// three float4 rows, likewise transposed for HLSL's column-major float3x3 packing.
static void SetNormalMatrix(IDirect3DDevice9* device, u32 reg, const Matrix34& world)
{
    const Vector3& a = world.m0;
    const Vector3& b = world.m1;
    const Vector3& c = world.m2;

    // Cofactors give adjugate = det * inverse; since the shader normalises the result, the overall
    // determinant scale is irrelevant and the divide can be skipped entirely.
    Vector3 r0 {(b.y * c.z) - (b.z * c.y), (b.z * c.x) - (b.x * c.z), (b.x * c.y) - (b.y * c.x)};
    Vector3 r1 {(c.y * a.z) - (c.z * a.y), (c.z * a.x) - (c.x * a.z), (c.x * a.y) - (c.y * a.x)};
    Vector3 r2 {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};

    // (cofactor matrix) is already the inverse-transpose up to scale. Transpose for HLSL packing.
    const f32 values[12] {
        r0.x, r1.x, r2.x, 0.0f, //
        r0.y, r1.y, r2.y, 0.0f, //
        r0.z, r1.z, r2.z, 0.0f};

    device->SetVertexShaderConstantF(reg, values, 3);
}

static void SetVec4(IDirect3DDevice9* device, u32 reg, f32 x, f32 y, f32 z, f32 w)
{
    const f32 values[4] {x, y, z, w};
    device->SetPixelShaderConstantF(reg, values, 1);
}

// ---------------------------------------------------------------------------------------------
// Per-frame light pool and cluster grid
//
// Everything below runs ONCE per frame, from UpdateLights(). It used to run once per DRAW, as a
// gather-cull-sort over every live light producing sixteen constant registers - which cost CPU
// proportional to (meshes x lights) and capped the shader at however many lights would fit in an
// unrolled loop. See the note on agiDX9ClusterGrid in dx9shader.h for why the texture-plus-grid
// form lifts both.
// ---------------------------------------------------------------------------------------------

namespace
{
    struct PooledLight
    {
        Vector3 Position;
        Vector3 Color;
        f32 Reach;
        f32 Energy;
    };

    // Reach beyond which a light is clamped for grid insertion purposes.
    //
    // agiLighter::LIGHTS entries with no attenuation curve fall back to a nominal 1000-unit range,
    // and a light that large would be inserted into every cell of the grid - defeating the whole
    // point of clustering while costing 8192 bucket writes on its own. Nothing in this game is
    // physically a 1000-unit light, so clamping is the honest reading rather than a compromise; it
    // is applied to the shader's attenuation window too, so the grid and the shading agree about
    // where the light stops.
    constexpr f32 kMaxLightReach = 128.0f;

    // Scratch for the grid build. File-scope rather than automatic because it is half a megabyte -
    // and it is rebuilt from scratch every frame, so nothing carries over between frames.
    u8 g_CellCounts[agiDX9ClusterGrid::CellCount];
    f32 g_CellSlots[agiDX9ClusterGrid::CellCount * agiDX9ClusterGrid::LightsPerCell];

    // Twice MaxLights, so the ranking below has something to choose from when the frame has more
    // lights than the pool can hold rather than simply keeping whichever arrived first.
    constexpr u32 kPoolCapacity = agiDX9ClusterGrid::MaxLights * 2;

    PooledLight g_Pool[kPoolCapacity];
} // namespace

// Positive modulo, matching the shader's `c - floor(c / dim) * dim`. std::fmod and C's % both
// truncate toward zero, which gives the wrong cell for negative world coordinates - and half the
// city is at negative X or Z, so getting this wrong would break clustering across the origin only.
static inline i32 WrapCell(i32 index, i32 dim)
{
    const i32 r = index % dim;
    return (r < 0) ? (r + dim) : r;
}

// Collects the frame's lights from both sources - the engine's own agiLighter::LIGHTS and the
// harvested glow registry - resolving each glow's colour and per-kind intensity as it goes.
static u32 BuildLightPool(PooledLight* out, u32 max_out)
{
    u32 count = 0;

    for (i32 i = 0; (i < agiLighter::Current) && (i < agiLighter::MAX_LIGHTS) && (count < max_out); ++i)
    {
        agiLight* light = agiLighter::LIGHTS[i];

        if (!light)
            continue;

        const agiLightParameters& params = light->Params;

        // Directional lights (Position.w == 0) are the static rig's business, not this list's.
        if (params.Position.w == 0.0f)
            continue;

        // Derive a finite range from the attenuation curve - the distance at which the light has
        // fallen to roughly 1/256, below which it cannot affect an 8-bit target.
        f32 reach = kMaxLightReach;

        if (params.QuadraticAtten > 0.0f)
            reach = std::sqrt(256.0f / params.QuadraticAtten);
        else if (params.LinearAtten > 0.0f)
            reach = 256.0f / params.LinearAtten;

        const Vector3 color = params.Diffuse;

        out[count++] = {Vector3 {params.Position.x, params.Position.y, params.Position.z}, color,
            std::min(reach, kMaxLightReach), color.x + color.y + color.z};
    }

    // Glow billboards and glow meshes as real light sources - street lamps, traffic signals, lit
    // signage, coronas, head/tail/brake lights. See agiworld/glowlight.h. These are the bulk of the
    // city's *apparent* light at night, and the original engine emits nothing from any of them.
    if (!PARAM_d3d9_glowlights.get_or(true))
        return count;

    const f32 power = PARAM_d3d9_glowpower.get_or(1.5f);

    for (u32 i = 0; (i < agiGlowLightCount) && (count < max_out); ++i)
    {
        const agiGlowLight& glow = agiGlowLights[i];

        // Extrapolate to where the light is NOW.
        //
        // A slot holds the position from the last frame its sprite was drawn. This pool is built at
        // BeginFrame, before any of this frame's sprites have been harvested, so every entry is at
        // least one frame old by construction and at speed that is the light pool visibly trailing
        // the car. Age is exactly the number of frames to extrapolate over.
        const Vector3 light_pos = glow.Position + (glow.Velocity * static_cast<f32>(glow.Age));

        // Fade a light out as it ages towards expiry, so one whose sprite stops being drawn leaves
        // smoothly instead of blinking off. See agiGlowLightFade.
        const f32 fade = agiGlowLightFade(glow.Age);

        if (fade <= 0.0f)
            continue;

        Vector3 color = glow.Tint * fade;

        // The flare's own hue, sampled out of the glow texture on the coarse grid that keeps a
        // traffic signal's red, amber and green apart. This is where the light's COLOUR actually
        // comes from; the tint above only modulates it.
        f32 intensity = glow.Intensity;

        if (glow.Texture)
        {
            agiDX9TexDef* tex = static_cast<agiDX9TexDef*>(glow.Texture);

            if (tex->HasGlowColors())
            {
                // Component-wise; Vector3 only defines a scalar multiply.
                const Vector3 hue = tex->SampleGlowColor(glow.U, glow.V);
                color = {color.x * hue.x, color.y * hue.y, color.z * hue.z};

                // Re-classify against the RESOLVED colour rather than the tint alone.
                //
                // The harvester (agiAddGlowLightRGB) can only see the card/mesh vertex colour,
                // because sampling the texture needs the renderer. That routinely disagrees with
                // what the flare actually emits: a lamp drawn with a plain white vertex colour over
                // a warm amber sheet was being classified as white. Only the renderer knows both, so
                // this is the right place to settle it.
                intensity = agiClassifyGlowIntensity(tex->Tex.Name, color);
            }
        }

        const f32 reach = std::min(glow.Radius, kMaxLightReach);

        // Intensity scales with the SQUARE of reach, which is what a bigger fixture physically is:
        // a street lamp is not a brighter tail light, it is a far more powerful source mounted
        // further away. Attenuation is windowed inverse-square with its denominator clamped at one
        // unit, so a fixed intensity would give the same brightness at 1 unit however far the light
        // is meant to throw - and by the time you are 8 units under a lamp with a 24-unit reach,
        // that is about 1% of it. With linear scaling a lamp 8 m above the road delivered about
        // 0.05: visible in a histogram and nowhere else, which is why lamps never lit the street.
        //
        // Uncapped, because ACES tonemapping is on by default: overbright right at the bulb rolls
        // off instead of clipping, which is also what looking at a real lamp does. The 6-unit
        // reference keeps a small car lamp near the raw power value.
        const f32 reach_ref = std::max(reach, 1.0f) / 6.0f;
        const f32 gain = power * reach_ref * reach_ref * intensity;

        const Vector3 emitted = color * gain;

        out[count++] = {light_pos, emitted, std::max(reach, 1.0f), emitted.x + emitted.y + emitted.z};
    }

    return count;
}

void agiDX9WorldShader::UpdateLights(IDirect3DDevice9* device)
{
    light_count_ = 0;
    cell_fill_ = 0;

    if (!valid_ || !light_tex_ || !cell_tex_)
        return;

    cell_size_ = std::max(PARAM_d3d9_cellsize.get_or(24.0f), 1.0f);

    u32 pool_count = BuildLightPool(g_Pool, kPoolCapacity);

    // Rank by emitted energy, descending. Two things depend on the order: which lights survive if
    // the frame has more than MaxLights of them, and - more often - which survive a bucket that has
    // more than LightsPerCell lights over it. Inserting strongest-first means an overfull junction
    // keeps its street lamps and drops the tail light of a car three cars back, which is the right
    // way round. (The old per-draw sort ranked by energy/distance^2 to the receiving mesh; there is
    // no single receiver here, and distance is what the grid is for.)
    std::sort(g_Pool, g_Pool + pool_count,
        [](const PooledLight& a, const PooledLight& b) { return a.Energy > b.Energy; });

    if (pool_count > agiDX9ClusterGrid::MaxLights)
        pool_count = agiDX9ClusterGrid::MaxLights;

    // --- Light data texture --------------------------------------------------------------------
    D3DLOCKED_RECT locked;

    if (SUCCEEDED(light_tex_->LockRect(0, &locked, nullptr, 0)))
    {
        u8* base = static_cast<u8*>(locked.pBits);

        f32* row_pos = reinterpret_cast<f32*>(base);
        f32* row_col = reinterpret_cast<f32*>(base + locked.Pitch);

        for (u32 i = 0; i < pool_count; ++i)
        {
            const PooledLight& light = g_Pool[i];

            row_pos[(i * 4) + 0] = light.Position.x;
            row_pos[(i * 4) + 1] = light.Position.y;
            row_pos[(i * 4) + 2] = light.Position.z;
            row_pos[(i * 4) + 3] = 1.0f / std::max(light.Reach * light.Reach, 1e-4f);

            row_col[(i * 4) + 0] = light.Color.x;
            row_col[(i * 4) + 1] = light.Color.y;
            row_col[(i * 4) + 2] = light.Color.z;
            row_col[(i * 4) + 3] = 0.0f;
        }

        light_tex_->UnlockRect(0);
    }

    // --- Cluster assignment --------------------------------------------------------------------
    std::memset(g_CellCounts, 0, sizeof(g_CellCounts));

    const f32 inv_cell = 1.0f / cell_size_;

    for (u32 i = 0; i < pool_count; ++i)
    {
        const PooledLight& light = g_Pool[i];

        // Cells overlapped by the light's sphere. Spans are computed unwrapped and wrapped per cell,
        // so a light straddling the grid seam lands in both halves rather than in neither.
        const i32 x0 = static_cast<i32>(std::floor((light.Position.x - light.Reach) * inv_cell));
        const i32 x1 = static_cast<i32>(std::floor((light.Position.x + light.Reach) * inv_cell));
        const i32 y0 = static_cast<i32>(std::floor((light.Position.y - light.Reach) * inv_cell));
        const i32 y1 = static_cast<i32>(std::floor((light.Position.y + light.Reach) * inv_cell));
        const i32 z0 = static_cast<i32>(std::floor((light.Position.z - light.Reach) * inv_cell));
        const i32 z1 = static_cast<i32>(std::floor((light.Position.z + light.Reach) * inv_cell));

        // A span wider than the grid would visit the same bucket repeatedly; one full turn covers
        // every cell on that axis exactly once, which is the same result for less work.
        const i32 nx = std::min(x1 - x0, static_cast<i32>(agiDX9ClusterGrid::DimX) - 1);
        const i32 ny = std::min(y1 - y0, static_cast<i32>(agiDX9ClusterGrid::DimY) - 1);
        const i32 nz = std::min(z1 - z0, static_cast<i32>(agiDX9ClusterGrid::DimZ) - 1);

        for (i32 dz = 0; dz <= nz; ++dz)
        {
            const i32 cz = WrapCell(z0 + dz, static_cast<i32>(agiDX9ClusterGrid::DimZ));

            for (i32 dy = 0; dy <= ny; ++dy)
            {
                const i32 cy = WrapCell(y0 + dy, static_cast<i32>(agiDX9ClusterGrid::DimY));

                const i32 plane = ((cz * static_cast<i32>(agiDX9ClusterGrid::DimY)) + cy) *
                    static_cast<i32>(agiDX9ClusterGrid::DimX);

                for (i32 dx = 0; dx <= nx; ++dx)
                {
                    const i32 cell = plane + WrapCell(x0 + dx, static_cast<i32>(agiDX9ClusterGrid::DimX));

                    u8& fill = g_CellCounts[cell];

                    if (fill >= agiDX9ClusterGrid::LightsPerCell)
                        continue;

                    g_CellSlots[(static_cast<u32>(cell) * agiDX9ClusterGrid::LightsPerCell) + fill] =
                        static_cast<f32>(i);

                    ++fill;
                    ++cell_fill_;
                }
            }
        }
    }

    // --- Bucket texture ------------------------------------------------------------------------
    if (SUCCEEDED(cell_tex_->LockRect(0, &locked, nullptr, 0)))
    {
        u8* base = static_cast<u8*>(locked.pBits);

        for (u32 row = 0; row < agiDX9ClusterGrid::TexHeight; ++row)
        {
            f32* dst = reinterpret_cast<f32*>(base + (static_cast<usize>(row) * locked.Pitch));

            for (u32 col = 0; col < agiDX9ClusterGrid::CellsPerRow; ++col)
            {
                const u32 cell = (row * agiDX9ClusterGrid::CellsPerRow) + col;
                const u32 fill = g_CellCounts[cell];

                const f32* src = &g_CellSlots[cell * agiDX9ClusterGrid::LightsPerCell];
                f32* out = &dst[col * agiDX9ClusterGrid::LightsPerCell];

                for (u32 slot = 0; slot < agiDX9ClusterGrid::LightsPerCell; ++slot)
                {
                    // -1 terminates, and the shader breaks on the first one. Every unused slot is
                    // still written rather than just the first: this is a persistent D3DPOOL_MANAGED
                    // texture reused every frame, so a stale index left behind by a busier frame
                    // would be read as a live light.
                    out[slot] = (slot < fill) ? src[slot] : -1.0f;
                }
            }
        }

        cell_tex_->UnlockRect(0);
    }

    light_count_ = pool_count;

    // Bind the two lighting textures for the whole frame rather than per draw. Stages 1 and 2 are
    // untouched by everything else in this backend - agiCurState::GetMaxTextures() is pinned to 1,
    // so the fixed-function path never enables a second stage and cannot sample them - and binding
    // here keeps the per-draw path down to constants only.
    device->SetTexture(1, light_tex_);
    device->SetTexture(2, cell_tex_);

    for (DWORD stage = 1; stage <= 2; ++stage)
    {
        // POINT and CLAMP, unconditionally: these textures are lookup tables, and any filtering
        // would blend a light index with its neighbour and fabricate a light that does not exist.
        device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
}

static D3DMATRIX ToD3D(const Matrix34& m)
{
    D3DMATRIX r;

    r._11 = m.m0.x; r._12 = m.m0.y; r._13 = m.m0.z; r._14 = 0.0f;
    r._21 = m.m1.x; r._22 = m.m1.y; r._23 = m.m1.z; r._24 = 0.0f;
    r._31 = m.m2.x; r._32 = m.m2.y; r._33 = m.m2.z; r._34 = 0.0f;
    r._41 = m.m3.x; r._42 = m.m3.y; r._43 = m.m3.z; r._44 = 1.0f;

    return r;
}

// Reproduces BuildProjectionMatrix() from dx9rsys.cpp. Duplicated rather than shared because the
// two paths are free to diverge - Pathway B has no reason to keep matching agiMeshSet::DepthScale
// once it stops sharing a depth buffer with pretransformed content.
static D3DMATRIX BuildProj(const agiViewParameters& p)
{
    D3DMATRIX r {};

    r._11 = p.ProjX;
    r._22 = p.ProjY;

    if (agiMeshSet::FlipX)
        r._11 = -r._11;

    r._33 = -p.ProjZZ * agiMeshSet::DepthScale + agiMeshSet::DepthOffset;
    r._34 = 1.0f;
    r._43 = p.ProjZW * agiMeshSet::DepthScale;

    return r;
}

void agiDX9WorldShader::Setup(IDirect3DDevice9* device, const agiDX9WorldDrawInfo& info)
{
    const agiDX9WorldMaterial material = agiDX9ResolveMaterial(info.Texture, !info.StaticLighting);

    device->SetVertexDeclaration(decl_);
    device->SetVertexShader(vs_);
    device->SetPixelShader((info.Lit && !material.ForceUnlit) ? ps_lit_ : ps_unlit_);

    // --- Vertex constants ----------------------------------------------------------------------
    D3DMATRIX world = ToD3D(*info.World);
    D3DMATRIX view = ToD3D(*info.ViewZFlip);
    D3DMATRIX proj = BuildProj(*info.Proj);

    D3DMATRIX world_view;
    Mul4x4(world_view, world, view);

    D3DMATRIX wvp;
    Mul4x4(wvp, world_view, proj);

    SetMatrix(device, 0, wvp);
    SetMatrix(device, 4, world);
    SetNormalMatrix(device, 8, *info.World);

    // --- Pixel constants -----------------------------------------------------------------------
    SetVec4(device, 0, material.Roughness, material.Metalness, material.Emissive, material.AoAmount);


    agiFogMode fog_mode = agiCurState.GetFogMode();

    // Additively-composited AlphaGlow content must not be fogged. Fogging happens before the
    // One_One blend, so it turns the glow texture's black background into fogColor*f and that gets
    // added across the whole quad - drawing a visible rectangle around every flare. The engine
    // suppresses fog for these itself on the CPU path (FirstPass_*, gated on DisableFogOnAlphaGlow);
    // the fixed-function branch of this backend does the same via IsAdditiveGlow() in dx9rsys.cpp.
    const bool glow = info.Texture && (info.Texture->Tex.Props & agiTexProp::AlphaGlow);

    bool fog_enabled = (fog_mode != agiFogMode::None) && !glow;

    // The engine expresses its fog range two different ways depending on renderer flags. Pixel-fog
    // mode puts real world-unit start/end into agiCurState; vertex-fog mode instead bakes a scale
    // into agiMeshSet::FogValue (= 255 / fog_end) and stores nothing useful in agiCurState. Recover
    // whichever is live so this path fogs at the same distance either way.
    f32 fog_start = agiCurState.GetFogStart();
    f32 fog_end = agiCurState.GetFogEnd();

    if (fog_mode == agiFogMode::Vertex)
    {
        fog_start = 1.0f;
        fog_end = (agiMeshSet::FogValue > 0.0f) ? (255.0f / agiMeshSet::FogValue) : 1000.0f;
    }

    // Height falloff defaults to 0, i.e. off, so out of the box this path's fog is exactly the
    // engine's own linear ramp and nothing more. Height fog is a genuine improvement over what
    // fixed-function table fog can express, but it is an *addition* to the artist-authored look
    // (mmEnvSetup's FogEnd is 200-500 world units depending on weather, and 0 - meaning no fog at
    // all - for every clear preset), so it has to be opted into rather than imposed.
    SetVec4(device, 1, fog_start, fog_end, PARAM_d3d9_heightfog.get_or(0.0f), fog_enabled ? 1.0f : 0.0f);

    u32 fog_color = agiCurState.GetFogColor();
    SetVec4(device, 2, ((fog_color >> 16) & 0xFF) / 255.0f, ((fog_color >> 8) & 0xFF) / 255.0f,
        (fog_color & 0xFF) / 255.0f, 1.0f);

    const Vector3& camera = info.Proj->Camera.m3;
    SetVec4(device, 3, camera.x, camera.y, camera.z, PARAM_d3d9_exposure.get_or(1.0f));

    // Hemisphere environment.
    //
    // An earlier revision took the sky colour from agiCurState's fog colour, on the theory that
    // mmCullCity::Cull() sets it to the current SkyColor and it would therefore track time of day
    // and weather for free. Reading the actual preset table (mmEnvSetup, mmcity/cullcity.cpp)
    // shows that is wrong in two ways that compound badly:
    //
    //   * SkyColor is only meaningful for three of the four weather presets. Clear weather has
    //     FogEnd == 0, so Cull() takes its "no fog" branch and never calls SetFogColor() at all -
    //     the value left in agiCurState is stale. Rain's SkyColor is literally 0x000000 and Snow's
    //     is 0xFFFFFF; neither describes ambient light.
    //   * Because the hemisphere term feeds the *indirect* lighting, it applies at every distance,
    //     not just far away. Driving it from the fog colour therefore painted a flat, fog-coloured
    //     wash over every surface in the frame regardless of how close it was - which is what made
    //     the fog read as far too intense. It was also double-counted: the ambient below was added
    //     on top of a hemisphere already built from it.
    //
    // The engine's own ambient is the correct source, and it already tracks the presets (fix_sun /
    // fix_fill1 / fix_fill2 drive it). Split into up/down lobes so surfaces still get directional
    // shape, with the total energy kept close to the flat ambient the CPU rig applies.
    const Vector3& ambient = info.StaticLighting ? agiMeshLighterAmbient : agiLighter::SceneAmbient;

    // Squared into the linear space the shader lights in, matching the albedo decode.
    const f32 amb_r = ambient.x * ambient.x;
    const f32 amb_g = ambient.y * ambient.y;
    const f32 amb_b = ambient.z * ambient.z;

    // .w is the tonemap enable - see the note at the end of world.ps.hlsl.
    //
    // Now ON by default. It was off while the only light sources were the engine's three
    // directionals, whose colours are authored as 0..1 multipliers - there was no excess range to
    // reclaim and the curve just flattened midtones. Glow-driven point lights change that: they are
    // genuine HDR sources whose sum near a lamp legitimately exceeds 1.0, and without a curve that
    // clips to flat white instead of rolling off.
    SetVec4(device, 4, 0.0f, 0.0f, 0.0f, PARAM_d3d9_tonemap.get_or(true) ? 1.0f : 0.0f);
    // Lightning: a broad, sky-dominant burst added to the hemisphere irradiance.
    //
    // Added to the environment term rather than as a directional light because that is what a strike
    // is optically - the whole sky becomes a light source for a moment, so surfaces facing up
    // brighten most and nothing casts a single hard shadow. Weighted 3:1 sky over ground for the
    // same reason. See agiLightningFlash (agiworld/glowlight.h) for why it has to be latched.
    const f32 flash = agiLightningFlash * PARAM_d3d9_flashpower.get_or(4.0f);

    SetVec4(device, 5, (amb_r * 1.35f) + flash, (amb_g * 1.35f) + flash, (amb_b * 1.35f) + flash, 1.0f);
    SetVec4(device, 6, (amb_r * 0.50f) + (flash * 0.33f), (amb_g * 0.50f) + (flash * 0.33f),
        (amb_b * 0.50f) + (flash * 0.33f), 1.0f);

    // The static sun/fill/fill rig, in world space. agiMeshLighter* directions already point toward
    // the light, which is what the shader wants - no negation here, unlike the D3DLIGHT9 path in
    // dx9rsys.cpp, whose directions describe travel instead.
    Vector3 dirs[3] {agiMeshLighterSun, agiMeshLighterFill1, agiMeshLighterFill2};
    Vector3 cols[3] {agiMeshLighterSunColor, agiMeshLighterFill1Color, agiMeshLighterFill2Color};

    // Replace the engine's sun with one that actually tracks the time of day and the weather.
    //
    // The original moves the sun in ONE axis and only for three of the four presets. fix_sun()
    // (game.asm ~178055) builds the direction from an elevation and an azimuth held in two globals,
    // but the azimuth is written once, to zero, and never again - so the sun only ever rises and
    // sets in a single plane. The elevation comes from a jump table on MMSTATE.TimeOfDay that sets
    // Morning to 0.70 rad, Noon to 1.396 and Sunset to 2.618, and has no case for Night at all,
    // which leaves whatever the previous preset wrote still standing. Weather never touches it.
    //
    // For a per-pixel path that is the wrong key light in three ways at once: the sun does not move
    // across the sky, night is lit by a stale daytime sun, and an overcast sky is as hard and as
    // warm as a clear one. All three are very visible on car paint, which is the one surface in the
    // game whose whole appearance is a specular response to the key light.
    //
    // Pathway B only, deliberately: agiMeshLighterSun is still what the CPU rig and the D3DLIGHT9
    // fixed-function path read, and rewriting the global would change Pathway A's output, which is
    // the parity baseline (dx9_rendering_pathways.md §2). -d3d9sun 0 restores the engine's own.
    if (PARAM_d3d9_sun.get_or(true))
        agiDX9ComputeSunLight(dirs[0], cols[0]);

    // Refresh the reflection probe from the same sun and sky this frame is lit by, so what a car
    // reflects and what lights it cannot disagree. Update() is a no-op unless something moved.
    Probe.Update(dirs[0], cols[0], Vector3 {amb_r * 1.35f, amb_g * 1.35f, amb_b * 1.35f},
        Vector3 {amb_r * 0.50f, amb_g * 0.50f, amb_b * 0.50f});

    if (Probe.IsValid())
    {
        device->SetTexture(3, Probe.GetTexture());

        device->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

        SetVec4(device, 16, Probe.GetMipCount() - 1.0f, PARAM_d3d9_reflect.get_or(1.0f), agiNativeReflectivity, 1.0f);

        // The vehicle's own authored sphere map, when this draw is a car body and the game supplied
        // one. Preferred over the synthesised probe for vehicles: it is the art the cars were built
        // against, and MM1 authored it precisely so bodywork would have something to mirror.
        //
        // agiNativeReflectionTex is set for the duration of one DrawLitSph and cleared after it, so
        // this is null for everything that is not a car.
        IDirect3DTexture9* sphere = nullptr;

        if (agiNativeReflectionTex)
        {
            sphere = static_cast<agiDX9TexDef*>(agiNativeReflectionTex)->GetHandle();

            // Report the reflection map whenever it changes. mmEnvSetup (mmcity/cullcity.cpp) holds
            // a 4x4 TimeOfDay x Weather table with its own refl_* texture per cell - refl_nc for
            // Noon/Clear, refl_sc for Sunset/Clear, refl_dc for Night/Clear and so on - and
            // aiVehicleInstance::Draw passes CullCity()->SphereMap, so the right one should follow
            // the preset. Logging it is how you confirm that rather than assume it.
            static agiTexDef* reported = nullptr;

            if (agiNativeReflectionTex != reported)
            {
                reported = agiNativeReflectionTex;
                Displayf("DX9 reflection map: '%s'", agiNativeReflectionTex->Tex.Name);
            }
        }

        if (sphere)
        {
            device->SetTexture(4, sphere);

            device->SetSamplerState(4, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(4, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(4, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(4, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(4, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }

        // Sphere-map coordinates are generated from the reflection vector in VIEW space, which is
        // the convention BuildVehicleReflectionVertices() worked out for the fixed-function pass
        // (dx9rsys.cpp) and which the authored texture is drawn for. The pixel shader has world
        // space, so hand it the view rotation transposed into columns - three dot products and no
        // extra interpolator.
        const Matrix34& v = *info.ViewZFlip;

        SetVec4(device, 17, v.m0.x, v.m1.x, v.m2.x, sphere ? 1.0f : 0.0f);
        SetVec4(device, 18, v.m0.y, v.m1.y, v.m2.y, 0.0f);
        SetVec4(device, 19, v.m0.z, v.m1.z, v.m2.z, 0.0f);
    }
    else
    {
        // w = 0 tells the shader there is no probe, so it keeps the flat hemisphere term.
        SetVec4(device, 16, 0.0f, 0.0f, 0.0f, 0.0f);
    }

    f32 light_dirs[12] {};
    f32 light_cols[12] {};

    for (i32 i = 0; i < 3; ++i)
    {
        light_dirs[(i * 4) + 0] = dirs[i].x;
        light_dirs[(i * 4) + 1] = dirs[i].y;
        light_dirs[(i * 4) + 2] = dirs[i].z;

        light_cols[(i * 4) + 0] = cols[i].x;
        light_cols[(i * 4) + 1] = cols[i].y;
        light_cols[(i * 4) + 2] = cols[i].z;
    }

    device->SetPixelShaderConstantF(7, light_dirs, 3);
    device->SetPixelShaderConstantF(10, light_cols, 3);

    // Cluster grid description. There is no per-draw light work left at all: the pool and the grid
    // were built once in UpdateLights(), and the pixel shader looks up its own cell from world
    // position. These two registers are constant for the frame and are re-issued here only because
    // nothing else guarantees a draw was preceded by one that set them.
    SetVec4(device, 13, static_cast<f32>(agiDX9ClusterGrid::DimX), static_cast<f32>(agiDX9ClusterGrid::DimY),
        static_cast<f32>(agiDX9ClusterGrid::DimZ), 1.0f / std::max(cell_size_, 1.0f));

    SetVec4(device, 14, static_cast<f32>(agiDX9ClusterGrid::CellsPerRow), static_cast<f32>(agiDX9ClusterGrid::TexWidth),
        static_cast<f32>(agiDX9ClusterGrid::TexHeight), PARAM_d3d9_lightspec.get_or(true) ? 1.0f : 0.0f);

    SetVec4(device, 15, 1.0f / static_cast<f32>(agiDX9ClusterGrid::MaxLights), 0.0f, 0.0f, 0.0f);

    // Fog is computed in the pixel shader (it needs world position for the height term, which
    // fixed-function fog has no way to express), so the fixed-function fog stage must be off or the
    // result would be fogged twice.
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);

    if (!info.Texture || !info.Texture->GetHandle())
        device->SetTexture(0, white_);
}

void agiDX9WorldShader::Unbind(IDirect3DDevice9* device)
{
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetVertexDeclaration(nullptr);

    // Restore the fixed-function fog state the rest of the frame expects. FlushState() caches fog
    // in agiLastState and only re-issues it on a change, so leaving it off here would silently
    // unfog every following CPU-path draw.
    device->SetRenderState(D3DRS_FOGENABLE, (agiLastState.FogMode != agiFogMode::None) ? TRUE : FALSE);
}

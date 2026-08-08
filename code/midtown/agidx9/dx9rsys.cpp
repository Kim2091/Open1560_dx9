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

#include "dx9rsys.h"

#include "agi/error.h"
#include "agi/light.h"
#include "agi/mtldef.h"
#include "agi/pipeline.h"
#include "agi/texdef.h"
#include "agi/viewport.h"
#include "agirend/lighter.h"
#include "agiworld/glowlight.h"
#include "agiworld/meshlight.h"
#include "agiworld/meshset.h"
#include "agiworld/texsheet.h" // agiTexProp::AlphaGlow
#include "data7/utimer.h"
#include "eventq7/active.h"
#include "memory/alloca.h"
#include "pcwindis/setupdata.h"
#include "vector7/matrix34.h"

#include "dx9context.h"
#include "dx9shader.h"
#include "dx9texdef.h"

#include "dx9_windows.h"

#include <algorithm>
#include <cmath>

define_dummy_symbol(agidx9_dx9rsys);

// D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1 - exactly matches agiScreenVtx's layout
static constexpr DWORD kScreenVtxFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1;
static constexpr DWORD kWorldReflectFVF = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// NOTE: at namespace scope deliberately. A function-local `static mem::cmd_param` is constructed
// lazily on first call, which happens long after mem::cmd_param::init() has already walked argv
// and assigned values to every registered parameter - so it registers too late and silently
// never receives its value, no matter what the user passes on the command line.
static mem::cmd_param PARAM_d3d9_depthbias {"d3d9depthbias", "Depth bias for hardware-transformed geometry"};

static u32 ImmPrimType = D3DPT_TRIANGLELIST;

static agiVtx* ImmVtxBase = nullptr;
static u32 ImmVtxCount = 0;

alignas(16) static u16 ImmIdxBuffer[4096];
static u32 ImmIdxCount = 0;

struct DX9ReflectVtx
{
    Vector3 pos;
    u32 color;
    f32 tu;
    f32 tv;
};

static ARTS_FORCEINLINE f32 Clamp01(f32 value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

static ARTS_FORCEINLINE Vector3 NormalizeSafe(const Vector3& value, const Vector3& fallback)
{
    f32 mag2 = value.Mag2();
    return (mag2 > 1.0e-8f) ? (value / std::sqrt(mag2)) : fallback;
}

static void BuildVehicleReflectionVertices(DX9ReflectVtx* output, agiWorldVtx* input, i32 count, const Matrix34& world,
    const Matrix34& view, const agiNativeMaterialFx& fx)
{
    Matrix34 view_zflip = view;
    view_zflip.m0.z = -view_zflip.m0.z;
    view_zflip.m1.z = -view_zflip.m1.z;
    view_zflip.m2.z = -view_zflip.m2.z;
    view_zflip.m3.z = -view_zflip.m3.z;

    Matrix34 world_view;
    world_view.Dot3x3(world, view_zflip);

    Vector3 sun_dir_camera;
    sun_dir_camera.Dot3x3(-agiMeshLighterSun, view_zflip);
    sun_dir_camera = NormalizeSafe(sun_dir_camera, {0.0f, 0.0f, 1.0f});

    constexpr f32 kHighlightExponent = 28.0f;

    for (i32 i = 0; i < count; ++i)
    {
        output[i].pos = input[i].pos;

        Vector3 view_pos;
        view_pos.Dot(input[i].pos, world_view);

        Vector3 normal_view;
        normal_view.Dot3x3(input[i].normal, world_view);
        normal_view = NormalizeSafe(normal_view, {0.0f, 0.0f, 1.0f});

        Vector3 view_dir = NormalizeSafe(-view_pos, {0.0f, 0.0f, 1.0f});
        f32 ndotv = Clamp01(normal_view ^ view_dir);

        Vector3 reflect_dir = (normal_view * (2.0f * ndotv)) - view_dir;
        reflect_dir = NormalizeSafe(reflect_dir, {0.0f, 0.0f, 1.0f});

        f32 m = 2.0f *
            std::sqrt((reflect_dir.x * reflect_dir.x) + (reflect_dir.y * reflect_dir.y) +
                ((reflect_dir.z + 1.0f) * (reflect_dir.z + 1.0f)));

        if (m > 1.0e-5f)
        {
            output[i].tu = Clamp01((reflect_dir.x / m) + 0.5f);
            output[i].tv = Clamp01(0.5f - (reflect_dir.y / m));
        }
        else
        {
            output[i].tu = 0.5f;
            output[i].tv = 0.5f;
        }

        f32 fresnel = fx.FresnelBias + (fx.FresnelScale * std::pow(1.0f - ndotv, 5.0f));

        Vector3 half_vec = NormalizeSafe(sun_dir_camera + view_dir, view_dir);
        f32 sun_glint = std::pow(Clamp01(normal_view ^ half_vec), kHighlightExponent) * 0.35f * fx.SpecularBoost;
        f32 intensity = Clamp01((fx.ReflectionAmount * fresnel) + sun_glint);
        u32 alpha = static_cast<u32>(Clamp01(intensity) * 255.0f);

        output[i].color = 0x00FFFFFF | (alpha << 24);
    }
}

static void DrawVehicleReflectionPass(IDirect3DDevice9* device, agiWorldVtx* vertices, i32 vertex_count, u16* indices,
    i32 index_count, const Matrix34& world, const Matrix34& view, const agiNativeMaterialFx& fx)
{
    if (!fx.ReflectionTexture)
        return;

    IDirect3DTexture9* reflection_texture = static_cast<agiDX9TexDef*>(fx.ReflectionTexture)->GetHandle();

    if (!reflection_texture)
        return;

    DX9ReflectVtx* reflect_verts = ARTS_ALLOCA(DX9ReflectVtx, vertex_count);
    BuildVehicleReflectionVertices(reflect_verts, vertices, vertex_count, world, view, fx);

    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    device->SetTexture(0, reflection_texture);
    device->SetFVF(kWorldReflectFVF);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vertex_count, index_count / 3, indices, D3DFMT_INDEX16,
        reflect_verts, sizeof(DX9ReflectVtx));

    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    // Restore what agiLastState believes, not an unconditional TRUE. FlushState() only re-issues
    // D3DRS_ZWRITEENABLE when agiCurState's value *changes*, so forcing it on here left depth
    // writes enabled behind the cache's back for every following draw that didn't happen to toggle
    // it - notably the alpha-blended sprite passes (glows, coronas, smoke) that deliberately run
    // with ZWrite off, which then punched their quads into the depth buffer and occluded geometry
    // behind them. MeshWorld()'s own restore block does not cover ZWrite, so it has to happen here.
    device->SetRenderState(D3DRS_ZWRITEENABLE, agiLastState.ZWrite ? TRUE : FALSE);

    agiLastState.Texture = nullptr;
}

agiDX9Rasterizer::agiDX9Rasterizer(agiPipeline* pipe)
    : agiRasterizer(pipe)
{}

agiDX9Rasterizer::~agiDX9Rasterizer() = default;

i32 agiDX9Rasterizer::BeginGfx()
{
    IDirect3DDevice9* device = Pipe()->Context()->GetDevice();

    // These never change for our purposes - our vertex format never carries per-vertex specular,
    // and lighting is only used by the (separate) world-space path.
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    device->SetRenderState(D3DRS_DITHERENABLE, TRUE);

    current_texture_ = nullptr;
    tex_env_ = agiTexEnv::Disable;

    return AGI_ERROR_SUCCESS;
}

void agiDX9Rasterizer::EndGfx()
{
    current_texture_ = nullptr;
}

void agiDX9Rasterizer::BeginGroup()
{}

void agiDX9Rasterizer::EndGroup()
{
    ImmDraw();
    ImmVtxBase = nullptr;
    ImmVtxCount = 0;
}

void agiDX9Rasterizer::Verts(agiVtxType type, agiVtx* vertices, i32 vertex_count)
{
    ArAssert(type == agiVtxType::Screen, "Invalid Vertex Type");

    ImmVtxBase = vertices;
    ImmVtxCount = vertex_count;
}

void agiDX9Rasterizer::Points(
    [[maybe_unused]] agiVtxType type, [[maybe_unused]] agiVtx* vertices, [[maybe_unused]] i32 vertex_count)
{}

void agiDX9Rasterizer::SetVertCount(i32 vertex_count)
{
    ImmVtxCount = vertex_count;
}

void agiDX9Rasterizer::Triangle(i32 i0, i32 i1, i32 i2)
{
    ++STATS.Tris;

    u16* indices = ImmAddIndices(D3DPT_TRIANGLELIST, 3);
    indices[0] = static_cast<u16>(i0);
    indices[1] = static_cast<u16>(i1);
    indices[2] = static_cast<u16>(i2);
}

void agiDX9Rasterizer::Line(i32 i0, i32 i1)
{
    ++STATS.Lines;

    u16* indices = ImmAddIndices(D3DPT_LINELIST, 2);
    indices[0] = static_cast<u16>(i0);
    indices[1] = static_cast<u16>(i1);
}

u16* agiDX9Rasterizer::ImmAddIndices(u32 prim_type, u16 count)
{
    if ((prim_type != ImmPrimType) || (ImmIdxCount + count > ARTS_SIZE(ImmIdxBuffer)))
    {
        ImmDraw();
        ImmPrimType = prim_type;
    }

    u16* result = &ImmIdxBuffer[ImmIdxCount];
    ImmIdxCount += count;
    return result;
}

void agiDX9Rasterizer::ImmDraw()
{
    if (ImmVtxCount && ImmIdxCount)
        DrawMesh(ImmPrimType, ImmVtxBase, ImmVtxCount, ImmIdxBuffer, std::exchange(ImmIdxCount, 0));
}

void agiDX9Rasterizer::Card(i32 v0, i32 v1)
{
    ++STATS.Cards;

    // Never implemented in any renderer (agigl's Card() is an identical no-op stub) - this is
    // our best evidence-based reconstruction: Card sits between Line(i0,i1) (2 vertex indices,
    // referencing the buffer set by Verts()) and Mesh() in the vtable, so it very likely draws a
    // simple screen-space sprite/billboard quad from 2 DIAGONAL CORNERS rather than needing all 4
    // corners spelled out - a common shortcut for flat sprites (light flares, checkpoint
    // billboards) where all 4 corners share one texture/depth. The 2 unlisted corners mix each
    // given corner's position axis with the OTHER corner's texture coordinate.
    if (!ImmVtxBase)
        return;

    // Flush any pending immediate-mode batch first to preserve draw order - this needs its own
    // small vertex buffer (2 synthesized corners) that doesn't exist in ImmVtxBase.
    ImmDraw();

    const agiScreenVtx& a = ImmVtxBase[v0].Screen;
    const agiScreenVtx& b = ImmVtxBase[v1].Screen;

    agiScreenVtx quad[4];
    quad[0] = a;
    quad[1] = a;
    quad[1].x = b.x;
    quad[1].tu = b.tu;
    quad[2] = b;
    quad[3] = b;
    quad[3].x = a.x;
    quad[3].tu = a.tu;

    u16 indices[6] {0, 1, 2, 0, 2, 3};

    STATS.Tris += 2;

    DrawMesh(D3DPT_TRIANGLELIST, reinterpret_cast<agiVtx*>(quad), 4, indices, 6);
}

void agiDX9Rasterizer::Mesh(agiVtxType type, agiVtx* vertices, i32 vertex_count, u16* indices, i32 index_count)
{
    ArAssert(type == agiVtxType::Screen, "Invalid Vertex Type");

    STATS.Tris += index_count / 3;

    DrawMesh(D3DPT_TRIANGLELIST, vertices, vertex_count, indices, index_count);
}

static D3DCMPFUNC ToD3DCmpFunc(agiCmpFunc func)
{
    switch (func)
    {
        case agiCmpFunc::Never: return D3DCMP_NEVER;
        case agiCmpFunc::Less: return D3DCMP_LESS;
        case agiCmpFunc::Equal: return D3DCMP_EQUAL;
        case agiCmpFunc::LessEqual: return D3DCMP_LESSEQUAL;
        case agiCmpFunc::Greater: return D3DCMP_GREATER;
        case agiCmpFunc::Notequal: return D3DCMP_NOTEQUAL;
        case agiCmpFunc::GreaterEqual: return D3DCMP_GREATEREQUAL;
        case agiCmpFunc::Always: return D3DCMP_ALWAYS;
    }

    return D3DCMP_ALWAYS;
}

// Shared by FlushState() and MeshWorld(). Applies the sampler filters implied by `tex_filter`,
// plus (inside SetFilters) the texture's own wrap/clamp addressing.
static void ApplyTexFilters(agiDX9TexDef* texture, agiTexFilter tex_filter)
{
    if (texture->Tex.DisableMipMaps() && tex_filter > agiTexFilter::Bilinear)
        tex_filter = agiTexFilter::Bilinear;

    D3DTEXTUREFILTERTYPE min_filter = D3DTEXF_POINT;
    D3DTEXTUREFILTERTYPE mag_filter = D3DTEXF_POINT;
    D3DTEXTUREFILTERTYPE mip_filter = D3DTEXF_NONE;

    switch (tex_filter)
    {
        case agiTexFilter::Trilinear:
            min_filter = D3DTEXF_LINEAR;
            mag_filter = D3DTEXF_LINEAR;
            mip_filter = D3DTEXF_LINEAR;
            break;

        case agiTexFilter::Bilinear:
            min_filter = D3DTEXF_LINEAR;
            mag_filter = D3DTEXF_LINEAR;
            mip_filter = D3DTEXF_POINT;
            break;

        case agiTexFilter::Point:
            min_filter = D3DTEXF_POINT;
            mag_filter = D3DTEXF_POINT;
            mip_filter = D3DTEXF_POINT;
            break;
    }

    texture->SetFilters(min_filter, mag_filter, mip_filter);
}

// Shared by FlushState() and MeshWorld(): MeshWorld() sets stage/render state directly (it has to -
// see the DrawMode note there) and must be able to put the device back exactly as agiLastState
// describes it, so both places have to agree on what each state value means.
static void ApplyTexEnv(IDirect3DDevice9* device, agiTexEnv tex_env)
{
    switch (tex_env)
    {
        case agiTexEnv::Disable:
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            break;

        case agiTexEnv::Replace:
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            break;

        case agiTexEnv::Modulate:
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            break;
    }
}

// Shared by FlushState() and MeshWorld(), for the same reason ApplyTexEnv() is: the native
// transform path bypasses agiTexSorter::DoTexture() (still closed assembly, game.asm), which is
// what binds per-texture material state on the CPU path, so MeshWorld() has to program the blend
// mode itself rather than inherit whatever the last CPU-path draw happened to leave behind.
static void ApplyBlendSet(IDirect3DDevice9* device, agiBlendSet blend_set)
{
    D3DBLEND blend_s = D3DBLEND_ONE;
    D3DBLEND blend_d = D3DBLEND_ZERO;

    switch (blend_set)
    {
        case agiBlendSet::SrcAlpha_InvSrcAlpha:
            blend_s = D3DBLEND_SRCALPHA;
            blend_d = D3DBLEND_INVSRCALPHA;
            break;

        case agiBlendSet::SrcAlpha_One:
            blend_s = D3DBLEND_SRCALPHA;
            blend_d = D3DBLEND_ONE;
            break;

        case agiBlendSet::Zero_SrcAlpha:
            blend_s = D3DBLEND_ZERO;
            blend_d = D3DBLEND_SRCALPHA;
            break;

        case agiBlendSet::Zero_SrcColor:
            blend_s = D3DBLEND_ZERO;
            blend_d = D3DBLEND_SRCCOLOR;
            break;

        case agiBlendSet::One_One:
            blend_s = D3DBLEND_ONE;
            blend_d = D3DBLEND_ONE;
            break;

        default: Quitf("bad blend mode"); break;
    }

    device->SetRenderState(D3DRS_SRCBLEND, blend_s);
    device->SetRenderState(D3DRS_DESTBLEND, blend_d);
}

static D3DCULL ToD3DCull(agiCullMode cull_mode)
{
    switch (cull_mode)
    {
        case agiCullMode::CW: return D3DCULL_CW;
        case agiCullMode::CCW: return D3DCULL_CCW;
        default: return D3DCULL_NONE;
    }
}

// Same mapping, but with the two culling senses exchanged - for the hardware-transform path, whose
// triangles reach the rasterizer with the opposite orientation from the CPU pretransform path's.
//
// The cause is the Z reflection in MeshWorld()'s `view_zflip`. That negates the Z *column* of the
// view matrix (each row's .z member), which is what puts view-space depth into the positive-forward
// convention BuildProjectionMatrix() is tuned for. A single-axis negation is a reflection: its
// determinant is negative, so it reverses the orientation of every triangle passing through it. The
// CPU path reaches the same depth convention differently - agiMeshSet::M negates the Z *row*, and
// its screen mapping (InitViewport: HalfWidth = +w/2, HalfHeight = -h/2) contributes a second
// reflection through the Y flip - so the two paths end up with opposite screen-space winding even
// though they produce identical pixel positions.
//
// Confirmed directly: feeding agiCurState's cull mode through unmodified culled *front* faces, and
// the frame came back showing the inside of the city - back faces of building shells, the road
// missing entirely, and the FINISH banner rendering its texture mirrored. Exchanging the senses
// renders correctly. This is the same compensation agigl performs with its `flip_winding_` flag.
static D3DCULL ToD3DCullFlipped(agiCullMode cull_mode)
{
    switch (cull_mode)
    {
        case agiCullMode::CW: return D3DCULL_CCW;
        case agiCullMode::CCW: return D3DCULL_CW;
        default: return D3DCULL_NONE;
    }
}

void agiDX9Rasterizer::FlushState()
{
    if (!agiCurState.IsTouched())
        return;

    ARTS_UTIMED(agiStateChanges);
    ++STATS.StateChanges;

    IDirect3DDevice9* device = Pipe()->Context()->GetDevice();

    agiDX9TexDef* texture =
        (agiCurState.GetDrawMode() == agiDrawTextured) ? static_cast<agiDX9TexDef*>(agiCurState.GetTexture()) : nullptr;

    agiTexFilter tex_filter = agiCurState.GetTexFilter();

    if (texture != agiLastState.Texture)
    {
        agiLastState.Texture = texture;

        if (texture)
        {
            if (IDirect3DTexture9* handle = texture->GetHandle(); handle != current_texture_)
            {
                current_texture_ = handle;

                ++STATS.TextureChanges;
                STATS.TxlsXrfd += texture->SurfaceSize;

                if (current_texture_)
                {
                    ApplyTexFilters(texture, tex_filter);
                }
            }
        }
        else
        {
            current_texture_ = nullptr;
        }
    }

    if (tex_filter != agiLastState.TexFilter)
    {
        agiLastState.TexFilter = tex_filter;
    }

    bool zenable = agiEnableZBuffer && agiCurState.GetZEnable();

    if (zenable != agiLastState.ZEnable)
    {
        agiLastState.ZEnable = zenable;
        device->SetRenderState(D3DRS_ZENABLE, zenable ? D3DZB_TRUE : D3DZB_FALSE);
        ++STATS.StateChangeCalls;
    }

    if (zenable)
    {
        if (bool zwrite = agiCurState.GetZWrite(); zwrite != agiLastState.ZWrite)
        {
            agiLastState.ZWrite = zwrite;

            device->SetRenderState(D3DRS_ZWRITEENABLE, zwrite ? TRUE : FALSE);
            ++STATS.StateChangeCalls;
        }

        if (agiCmpFunc zfunc = agiCurState.GetZFunc(); zfunc != agiLastState.ZFunc)
        {
            agiLastState.ZFunc = zfunc;

            device->SetRenderState(D3DRS_ZFUNC, ToD3DCmpFunc(zfunc));
            ++STATS.StateChangeCalls;
        }
    }

    if (bool smooth_shading = agiCurState.GetSmoothShading(); smooth_shading != agiLastState.SmoothShading)
    {
        agiLastState.SmoothShading = smooth_shading;

        device->SetRenderState(D3DRS_SHADEMODE, smooth_shading ? D3DSHADE_GOURAUD : D3DSHADE_FLAT);
        ++STATS.StateChangeCalls;
    }

    if (agiDrawMode draw_mode = agiCurState.GetDrawMode(); draw_mode != agiLastState.DrawMode)
    {
        agiLastState.DrawMode = draw_mode;

        D3DFILLMODE fill_mode = D3DFILL_SOLID;

        switch (static_cast<agiFillMode>(draw_mode & agiDrawFillMask))
        {
            case agiFillMode::Point: fill_mode = D3DFILL_POINT; break;
            case agiFillMode::Wire: fill_mode = D3DFILL_WIREFRAME; break;
            case agiFillMode::Solid: fill_mode = D3DFILL_SOLID; break;
        }

        device->SetRenderState(D3DRS_FILLMODE, fill_mode);
        ++STATS.StateChangeCalls;
    }

    bool alpha_enable = agiCurState.GetAlphaEnable();

    if (texture)
        alpha_enable |= texture->Tex.HasAlpha() || texture->Tex.UseChromakey();

    u8 alpha_ref = agiCurState.GetAlphaRef();

    if (alpha_enable != agiLastState.AlphaEnable || alpha_ref != agiLastState.AlphaRef)
    {
        agiLastState.AlphaEnable = alpha_enable;
        agiLastState.AlphaRef = alpha_ref;

        device->SetRenderState(D3DRS_ALPHABLENDENABLE, alpha_enable ? TRUE : FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, alpha_enable ? TRUE : FALSE);
        ++STATS.StateChangeCalls;

        if (alpha_enable)
        {
            device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
            device->SetRenderState(D3DRS_ALPHAREF, alpha_ref);
            ++STATS.StateChangeCalls;
        }
    }

    if (agiBlendSet blend_set = agiCurState.GetBlendSet(); blend_set != agiLastState.BlendSet)
    {
        agiLastState.BlendSet = blend_set;

        ApplyBlendSet(device, blend_set);
        ++STATS.StateChangeCalls;
    }

    if (agiCullMode cull_mode = agiCurState.GetCullMode(); cull_mode != agiLastState.CullMode)
    {
        agiLastState.CullMode = cull_mode;

        // Our vertices are already in D3D9 screen space (D3DFVF_XYZRHW), so no winding
        // flip is needed here (unlike the OpenGL backend, which has to compensate for
        // OpenGL's opposite screen-space Y axis).
        device->SetRenderState(D3DRS_CULLMODE, ToD3DCull(cull_mode));
        ++STATS.StateChangeCalls;
    }

    if (agiTexEnv tex_env = texture ? agiCurState.GetTexEnv() : agiTexEnv::Disable; tex_env != agiLastState.TexEnv)
    {
        agiLastState.TexEnv = tex_env;

        if (tex_env != tex_env_)
        {
            tex_env_ = tex_env;

            ApplyTexEnv(device, tex_env);
        }

        ++STATS.StateChangeCalls;
    }

    agiFogMode fog_mode = agiCurState.GetFogMode();
    f32 fog_start = agiCurState.GetFogStart();
    f32 fog_end = agiCurState.GetFogEnd();
    f32 fog_density = agiCurState.GetFogDensity();

    if (fog_mode != agiLastState.FogMode || fog_start != agiLastState.FogStart || fog_end != agiLastState.FogEnd ||
        fog_density != agiLastState.FogDensity)
    {
        agiLastState.FogMode = fog_mode;
        agiLastState.FogStart = fog_start;
        agiLastState.FogEnd = fog_end;
        agiLastState.FogDensity = fog_density;

        device->SetRenderState(D3DRS_FOGENABLE, (fog_mode != agiFogMode::None) ? TRUE : FALSE);

        switch (fog_mode)
        {
            case agiFogMode::None: break;

            case agiFogMode::Pixel:
                // Table (per-pixel) fog for pretransformed vertices - interpolates using the
                // vertex's screen-space depth (agiScreenVtx::z), same source as the OpenGL
                // backend's GL_FOG_COORD path.
                device->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
                device->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
                device->SetRenderState(D3DRS_FOGSTART, *reinterpret_cast<const DWORD*>(&fog_start));
                device->SetRenderState(D3DRS_FOGEND, *reinterpret_cast<const DWORD*>(&fog_end));
                break;

            case agiFogMode::Vertex:
                // With both fog modes set to NONE, D3D9 uses the alpha component of the
                // per-vertex specular colour directly as the fog factor - exactly the value
                // already baked into agiScreenVtx::specular by the engine's own fog code.
                device->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
                device->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
                break;
        }
    }

    if (u32 fog_color = agiCurState.GetFogColor(); fog_color != agiLastState.FogColor)
    {
        agiLastState.FogColor = fog_color;

        device->SetRenderState(D3DRS_FOGCOLOR, fog_color & 0x00FFFFFF);
    }

    agiCurState.ClearTouched();
}

// Per-frame census of how geometry actually reaches the device, so "is anything still submitted
// pretransformed?" is answered by counting rather than by reading call sites - most of the engine
// that draws is still closed assembly, so a source audit alone cannot answer it.
// Pretransformed (XYZRHW) draws carry no world-space information and are invisible to RTX Remix
// as 3D geometry, no matter how they look on screen. Dumped by agiDX9Pipeline::EndFrame().
agiDX9SubmitCensus agiDX9Census {};

// Fog must be OFF for additively-composited AlphaGlow content, and the engine says so itself.
//
// FirstPass_* (game.asm, e.g. ~324982) does exactly this: for a texture with agiTexProp::AlphaGlow,
// when DisableFogOnAlphaGlow is set, it zeroes agiCurState+0x17 - which is state_+0x13, i.e.
// agiRendStateStruct::FogMode (agi/rsys.h: agiRendState is a b32 touched_ followed by the struct).
// Neither of this backend's draw paths reproduced that, so glows were being fogged.
//
// The visible result is the reported one: a highlighted rectangle around every flare. A glow is
// blended One_One, and its texture background is black precisely so that black contributes nothing
// under an additive blend. Fog breaks that invariant - it computes lerp(src, fogColor, f) *before*
// the blend, so the background stops being black and becomes fogColor*f, which is then added to the
// framebuffer across the whole quad. The flare itself hides it in the middle; at the edges, where
// there is nothing but background, the quad's own rectangle shows up as a glowing box. It gets
// worse with distance (f grows) and worse again where several particle quads overlap, because each
// one adds its own rectangle - which is why it reads as particles and fog interacting.
static bool IsAdditiveGlow(agiTexDef* texture)
{
    return texture && (static_cast<agiDX9TexDef*>(texture)->Tex.Props & agiTexProp::AlphaGlow);
}

void agiDX9Rasterizer::DrawMesh(u32 prim_type, agiVtx* vertices, i32 vertex_count, u16* indices, i32 index_count)
{
    if (!IsAppActive() || (vertex_count == 0) || (index_count == 0))
        return;

    FlushState();

    if ((current_texture_ == nullptr) && (tex_env_ != agiTexEnv::Disable))
        return;

    ARTS_UTIMED(agiRasterization);
    ++STATS.GeomCalls;

    if (prim_type == D3DPT_LINELIST)
    {
        ++agiDX9Census.ScreenLineCalls;
        agiDX9Census.ScreenLines += index_count / 2;
    }
    else
    {
        ++agiDX9Census.ScreenCalls;
        agiDX9Census.ScreenTris += index_count / 3;

        if (Pipe()->IsInScene())
        {
            ++agiDX9Census.ScreenCallsInScene;
            agiDX9Census.ScreenTrisInScene += index_count / 3;
        }
    }

    IDirect3DDevice9* device = Pipe()->Context()->GetDevice();

    device->SetTexture(0, current_texture_);
    device->SetFVF(kScreenVtxFVF);

    // See IsAdditiveGlow(). Only touched for glow draws, so ordinary geometry pays nothing.
    const bool glow = IsAdditiveGlow(agiCurState.GetTexture());

    if (glow)
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);

    i32 primitive_count = (prim_type == D3DPT_LINELIST) ? (index_count / 2) : (index_count / 3);

    device->DrawIndexedPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(prim_type), 0, vertex_count, primitive_count, indices,
        D3DFMT_INDEX16, vertices, sizeof(agiScreenVtx));

    // Restore to whatever FlushState() believes it left on the device. It caches fog in
    // agiLastState and only re-issues on a change, so leaving fog off here would silently unfog
    // every following draw that happened not to change fog state.
    if (glow)
    {
        device->SetRenderState(D3DRS_FOGENABLE, (agiLastState.FogMode != agiFogMode::None) ? TRUE : FALSE);
    }
}

// Matrix34 (vector7/matrix34.h) is already row-vector, D3D9-shaped (m0/m1/m2 = X/Y/Z basis
// rows, m3 = translation row) - this is a direct field embed, no transposition needed.
static D3DMATRIX ToD3DMatrix(const Matrix34& m)
{
    D3DMATRIX result;

    result._11 = m.m0.x;
    result._12 = m.m0.y;
    result._13 = m.m0.z;
    result._14 = 0.0f;

    result._21 = m.m1.x;
    result._22 = m.m1.y;
    result._23 = m.m1.z;
    result._24 = 0.0f;

    result._31 = m.m2.x;
    result._32 = m.m2.y;
    result._33 = m.m2.z;
    result._34 = 0.0f;

    result._41 = m.m3.x;
    result._42 = m.m3.y;
    result._43 = m.m3.z;
    result._44 = 1.0f;

    return result;
}

// Built directly from agiViewParameters' already-computed projection coefficients (no need to
// touch the still-imported agiViewParameters::Perspective() that derives them). Mathematically
// equivalent to the CPU M-multiply + ToScreen() every other renderer relies on: with row-vectors
// v' = v * World * View * Projection, the third row/fourth column here reproduce
// `z_clip = view_z * ProjZZ + ProjZW` and `w_clip = view_z` exactly as agiMeshSet::Transform() does.
//
// The CPU path's ToScreen() doesn't stop at `ProjZZ + ProjZW/view_z` - it additionally remaps
// that value through `* agiMeshSet::DepthScale + agiMeshSet::DepthOffset` (0.5/0.5 by default)
// before writing agiScreenVtx::z. That remap turns the engine's OpenGL-style [-1, 1] NDC depth
// into D3D9's [0, 1] depth range. Skipping it here would leave clip-space Z in the wrong range
// for D3D9's hardware near/far clipping, causing incorrect clipping of large surfaces (roads,
// building facades) that span a wide depth range.
static D3DMATRIX BuildProjectionMatrix(const agiViewParameters& p)
{
    // NOTE: ProjXZ/ProjYZ deliberately do NOT appear here. The actual mesh transform
    // (agiMeshSet::ARTS_TRANSFORM_DOT, meshrend.cpp) computes output.x/output.y purely from the
    // combined M matrix, with no ProjXZ/ProjYZ term anywhere - those two fields are only used by
    // DrawCard()/LineList() for an unrelated purpose (scaling a billboard/line's screen-space
    // width by its view-space Z). An earlier version of this function mistook that for a lens-shear
    // term and folded it into the projection matrix, which skewed every mesh (including city-instance
    // buildings and car wheels) whenever ProjXZ/ProjYZ happened to be non-zero (e.g. in action cam).
    D3DMATRIX result {};

    result._11 = p.ProjX;
    result._22 = p.ProjY;

    // agiMeshSet::FlipX mirrors the scene horizontally about the viewport centre. It is set by the
    // still-closed rear-view-mirror setup (game.asm ~180652, cleared again at ~180940), and the CPU
    // pretransform path honours it by negating agiMeshSet::HalfWidth in InitViewport() - which
    // negates NDC X while leaving OffsX, the viewport centre, alone. Nothing on this path applied
    // it, so mirror content drawn through MeshWorld came out un-mirrored while the CPU-path content
    // sharing the same rectangle (particles, sprites) stayed mirrored - the two disagreed about
    // left and right inside the same small box. Negating _11 is the exact projection-matrix
    // equivalent of negating HalfWidth. No winding compensation is needed because this path
    // rasterises with D3DCULL_NONE (see MeshWorld) - the CPU-side plane test already culled.
    if (agiMeshSet::FlipX)
        result._11 = -result._11;

    // agiViewParameters::ProjZZ is stored in the *negative-forward* (OpenGL) convention the
    // engine's own perspective setup produces - confirmed against the logged runtime values
    // (Near=2.997619, Far=1000 -> ProjZZ=-1.006013 == -(F+N)/(F-N), ProjZW=-6.013264 ==
    // -2FN/(F-N)). agiMeshSet's CPU path does not use that field directly: agiMeshSet::Init()
    // copies it in *negated* (game.asm, `fld [ProjZZ]` / `fchs` / `fstp [agiMeshSet::ProjZZ]`),
    // because ARTS_TRANSFORM_DOT's `output.w` comes from M's Z-row, which is likewise the
    // negation of Dot(World, View)'s Z-row - i.e. the CPU path works in positive-forward view
    // depth. ProjZW is copied across unnegated.
    //
    // MeshWorld() feeds D3D9 the same positive-forward convention (see view_zflip there), so the
    // matching coefficient here is -p.ProjZZ, not p.ProjZZ. Using the raw field made
    //     _33 = -1.006013 * 0.5 + 0.5 = -0.003007
    // which drives z_clip = _33 * view_z + _43 negative for *every* view_z > 0 (_43 is itself
    // negative), so every triangle submitted through this path failed D3D9's 0 <= z_clip <= w_clip
    // near-plane test and was clipped away entirely - geometry silently vanishing while the draw
    // call still reported success. With the sign corrected, z_clip reaches 0 exactly at view_z ==
    // Near (1.003007 * 2.997619 - 3.006632 ~= 0) and z_clip/w_clip reaches 1 at view_z == Far,
    // matching ToScreen()'s `z * inv_w * DepthScale + DepthOffset` term for term.
    result._33 = -p.ProjZZ * agiMeshSet::DepthScale + agiMeshSet::DepthOffset;
    result._34 = 1.0f;

    result._43 = p.ProjZW * agiMeshSet::DepthScale;

    return result;
}

static constexpr i32 kMaxWorldLights = 8;
static constexpr f32 kDegToRad = 3.14159265358979323846f / 180.0f;

static void SetupD3D9Lights(IDirect3DDevice9* device)
{
    i32 enabled = 0;

    // agiLighter::ACTIVELIGHTS is a per-object scratch cache written only by the still-closed
    // assembly CPU-lighting machinery (agiLighter::LightVertex and friends) - since this
    // world-space path bypasses CPU lighting entirely, that cache is never refreshed for us and
    // can hold dangling pointers left over from lights destroyed since the last CPU-lit draw
    // (this crashed with an access violation here after a race reset tore down its lights).
    // agiLighter::LIGHTS[0, Current) is instead maintained directly by real, reimplemented C++
    // (DeclareLight(), called from each agiLight's constructor), so it stays valid for the
    // lifetime of the lights it references - use that instead.
    for (i32 i = 0; i < agiLighter::Current && enabled < kMaxWorldLights; ++i)
    {
        agiLight* light = agiLighter::LIGHTS[i];

        if (!light)
            continue;

        const agiLightParameters& params = light->Params;

        bool positional = params.Position.w != 0.0f;
        bool spot = positional && params.SpotAngle < 180.0f;

        D3DLIGHT9 d3dlight {};
        d3dlight.Type = !positional ? D3DLIGHT_DIRECTIONAL : (spot ? D3DLIGHT_SPOT : D3DLIGHT_POINT);

        d3dlight.Diffuse = {params.Diffuse.x, params.Diffuse.y, params.Diffuse.z, params.Alpha};
        d3dlight.Ambient = {params.Ambient.x, params.Ambient.y, params.Ambient.z, 1.0f};
        d3dlight.Specular = {params.Specular.x, params.Specular.y, params.Specular.z, 1.0f};

        d3dlight.Position = {params.Position.x, params.Position.y, params.Position.z};
        d3dlight.Direction = {params.Direction.x, params.Direction.y, params.Direction.z};

        d3dlight.Range = 1.0e8f; // Effectively unbounded (D3DLIGHT_RANGE_MAX isn't in every SDK's headers)
        d3dlight.Falloff = 1.0f;
        d3dlight.Attenuation0 = params.ConstantAtten;
        d3dlight.Attenuation1 = params.LinearAtten;
        d3dlight.Attenuation2 = params.QuadraticAtten;

        if (spot)
        {
            // Best-effort mapping - the original (still-ARTS_IMPORT) lighting code may use a
            // different spot falloff curve. Using D3D9's own spot cone here, per the plan.
            d3dlight.Phi = std::clamp(params.SpotAngle * kDegToRad, 0.01f, 3.13f);
            d3dlight.Theta = d3dlight.Phi * 0.5f;
            d3dlight.Falloff = (params.SpotExp > 0.0f) ? params.SpotExp : 1.0f;
        }

        // The D3D9 light slot must be `enabled`, not `i`. agiLighter::LIGHTS is a 16-entry array
        // that is allowed to contain holes (RemoveLight() clears a slot without compacting it out),
        // and agiLighter::Current is its high-water mark, not a dense count. Indexing D3D9 by `i`
        // broke both ways. A hole before a live light left `enabled < i`, so the disable loop below
        // - which starts at `enabled` - immediately switched off a slot this loop had just filled,
        // and that light silently contributed nothing. And with more than 8 live lights `i` ran on
        // past kMaxWorldLights while `enabled` stopped at it, so slots 8..15 got enabled and were
        // never disabled again: they leaked into every later draw and exceeded the
        // D3DCAPS9::MaxActiveLights most drivers report.
        device->SetLight(static_cast<DWORD>(enabled), &d3dlight);
        device->LightEnable(static_cast<DWORD>(enabled), TRUE);

        ++enabled;
    }

    for (i32 i = enabled; i < kMaxWorldLights; ++i)
        device->LightEnable(static_cast<DWORD>(i), FALSE);
}

// The city's static rig has no specular term at all. agiMeshLighterTriple (agiworld/meshlight.cpp,
// mmxTriple) computes intensity = key + fill1 + fill2 + ambient and multiplies it into the vertex
// colour - three clamped N.L lobes and nothing else. There is no half-vector, no exponent, no
// specular colour anywhere in it.
//
// Adding one here gave every building facade and every road surface a moving highlight the original
// never had. It reads as wrong for three compounding reasons: the surfaces are flat and enormous,
// so the highlight sweeps across a whole wall at once; the lighting is per-vertex, so the highlight
// is interpolated across metre-scale triangles rather than resolved; and agiWorldVtx::normal comes
// from UnpackNormal[], a 198-entry quantised direction table, so adjacent facets that should share
// a normal often differ by several degrees and the specular power amplifies exactly that error into
// visible facet-by-facet banding. That is the reported "buildings and roads gleam and shine at
// weird angles".
//
// Pathway A is the parity path, so this is off by default. It stays available because it is a
// reasonable thing to want on a modern display - but per-pixel specular over real normals belongs
// to Pathway B (see docs/dx9_rendering_pathways.md), not to a per-vertex FF approximation.
static mem::cmd_param PARAM_d3d9_specular {"d3d9specular", "Add a specular term to the static city lighting rig"};

bool agiDX9WantsStaticSpecular()
{
    return PARAM_d3d9_specular.get_or(false);
}

// Builds the fixed sun/fill1/fill2 + ambient rig agiMeshLighterTriple/Quarter compute on the CPU
// for StaticLighter/DynamicLighter-lit content (city buildings/terrain - see meshrend.cpp's
// DrawLit) as three D3D9 directional lights, so hardware-transformed draws of that content are
// lit the same way instead of starved by the real-time dynamic light list (SetupD3D9Lights()/
// agiLighter::LIGHTS[]) built for cars/wheels - two genuinely different, unrelated light sources
// in the original engine (confirmed via disassembly: routing buildings through the dynamic list
// produced near-black output at night). Mirrors agiMeshLighterTriple's math exactly
// (agiworld/meshlight.cpp): per light, intensity = max(0, N . direction-to-light), modulated by
// the light's color, summed with a flat ambient term, then multiplied into the vertex color -
// D3D9's own directional lights plus a D3DMCS_COLOR1 diffuse source compute exactly this once the
// directions/colors/ambient below are set to match.
static void SetupD3D9StaticLights(IDirect3DDevice9* device)
{
    auto set_directional = [device](DWORD index, const Vector3& direction_to_light, const Vector3& color) {
        D3DLIGHT9 light {};
        light.Type = D3DLIGHT_DIRECTIONAL;
        light.Diffuse = {color.x, color.y, color.z, 1.0f};

        // Matches SetupD3D9StaticMaterial() - the original rig has no specular lobe, so by default
        // neither do these lights. See the note there.
        if (agiDX9WantsStaticSpecular())
            light.Specular = {color.x * 0.55f, color.y * 0.55f, color.z * 0.55f, 1.0f};
        // agiMeshLighterTriple's `direction` points toward the light source (meshlight.cpp); D3D9
        // directional lights instead specify the direction the light travels - the negation.
        light.Direction = {-direction_to_light.x, -direction_to_light.y, -direction_to_light.z};
        light.Range = 1.0e8f;

        device->SetLight(index, &light);
        device->LightEnable(index, TRUE);
    };

    set_directional(0, agiMeshLighterSun, agiMeshLighterSunColor);
    set_directional(1, agiMeshLighterFill1, agiMeshLighterFill1Color);
    set_directional(2, agiMeshLighterFill2, agiMeshLighterFill2Color);

    for (i32 i = 3; i < kMaxWorldLights; ++i)
        device->LightEnable(static_cast<DWORD>(i), FALSE);
}

// agiMeshLighterTriple has no material concept - it multiplies the summed sun/fill/ambient
// intensity directly into the vertex color, i.e. an implicit Ambient/Diffuse material of exactly
// (1,1,1,1). Deliberately ignores agiCurState.GetMtl() - that belongs to the dynamic
// agiLighter::LIGHTS[] path and its own materials, not this rig.
static void SetupD3D9StaticMaterial(IDirect3DDevice9* device)
{
    D3DMATERIAL9 d3dmtl {};
    d3dmtl.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
    d3dmtl.Ambient = {1.0f, 1.0f, 1.0f, 1.0f};

    if (agiDX9WantsStaticSpecular())
    {
        d3dmtl.Specular = {0.30f, 0.30f, 0.30f, 1.0f};
        d3dmtl.Power = 20.0f;
    }

    device->SetMaterial(&d3dmtl);
}

static void SetupD3D9Material(IDirect3DDevice9* device)
{
    D3DMATERIAL9 d3dmtl {};

    if (agiMtlDef* mtl = agiCurState.GetMtl())
    {
        const agiMtlParameters& params = mtl->Mtl;

        d3dmtl.Diffuse = {params.Diffuse.x, params.Diffuse.y, params.Diffuse.z, params.Diffuse.w};
        d3dmtl.Ambient = {params.Ambient.x, params.Ambient.y, params.Ambient.z, params.Ambient.w};
        d3dmtl.Specular = {params.Specular.x, params.Specular.y, params.Specular.z, params.Specular.w};
        d3dmtl.Emissive = {params.Emmisive.x, params.Emmisive.y, params.Emmisive.z, params.Emmisive.w};
        d3dmtl.Power = params.Power;
    }
    else
    {
        d3dmtl.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
        d3dmtl.Ambient = {1.0f, 1.0f, 1.0f, 1.0f};
        d3dmtl.Specular = {0.35f, 0.35f, 0.35f, 1.0f};
        d3dmtl.Power = 24.0f;
    }

    device->SetMaterial(&d3dmtl);
}

// Restores every piece of device state MeshWorld() programs, for both pathways. Shared so the
// fixed-function and programmable branches cannot drift apart on cleanup - which is the failure
// mode this whole file has been bitten by repeatedly (see the agiLastState note below).
void agiDX9Rasterizer::RestoreStateAfterWorldDraw(bool remap_vertex_fog)
{
    IDirect3DDevice9* device = Pipe()->Context()->GetDevice();

    // Pretransformed (agiScreenVtx) draws ignore D3DRS_LIGHTING entirely, but reset it anyway
    // for clarity - and so BeginGfx()'s initial state assumption keeps holding.
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);

    // Pretransformed draws are not lit either, so this has no effect on them - but leaving it set
    // would still be a lie about the device's state, and it costs the driver work on every
    // subsequent vertex. Put it back the way BeginGfx() left it.
    device->SetRenderState(D3DRS_NORMALIZENORMALS, FALSE);

    // Reset the depth bias so it doesn't leak into pretransformed (agiScreenVtx) draws, which
    // have no need for it and aren't tracked by FlushState()'s dirty-checking anyway.
    constexpr f32 kNoDepthBias = 0.0f;
    device->SetRenderState(D3DRS_DEPTHBIAS, *reinterpret_cast<const DWORD*>(&kNoDepthBias));

    // Undo the vertex-fog remap above. FlushState() caches fog state in agiLastState and only
    // re-issues it on a change, so table fog left switched on here would silently apply itself to
    // every following CPU-path draw - whose vertices carry the fog factor in specular alpha and
    // would then be fogged twice.
    if (remap_vertex_fog)
        device->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);

    // Fog enable is restored unconditionally, because two separate things above may have switched
    // it off for this draw - the vertex-fog remap and the additive-glow suppression - and the
    // programmable path switches it off too (dx9shader.cpp). One truthful restore covers all three.
    device->SetRenderState(D3DRS_FOGENABLE, (agiLastState.FogMode != agiFogMode::None) ? TRUE : FALSE);

    // Put the device back exactly as agiLastState describes it.
    //
    // Earlier revisions instead poisoned agiLastState (CullMode/Texture/TexEnv/AlphaEnable set to
    // impossible values) to force the next FlushState() to re-apply everything. That does not
    // work, and it is why alpha-keyed content went black across the city: FlushState() begins with
    //     if (!agiCurState.IsTouched()) return;
    // and `touched_` is only set when a *value actually changes* (agi/rsys.h, agiRendState::Set).
    // A following sprite draw that happens to want the same state the engine already had never
    // touches agiCurState, FlushState() returns immediately, and the poisoned agiLastState is
    // never even looked at - so the device silently kept this function's lighting-oriented setup.
    // Every glow and flare (taillights, traffic lights, street lamps, coronas) is drawn that way,
    // with additive/alpha blending the engine had set up long before, so they rasterised their
    // black texture background opaque and appeared as black boxes with the glow inside.
    //
    // Restoring the device to match agiLastState keeps that cache truthful, so it stays correct
    // whether or not FlushState() runs next.
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, agiLastState.AlphaEnable ? TRUE : FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, agiLastState.AlphaEnable ? TRUE : FALSE);

    if (agiLastState.AlphaEnable)
    {
        device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        device->SetRenderState(D3DRS_ALPHAREF, agiLastState.AlphaRef);
    }

    device->SetRenderState(D3DRS_CULLMODE, ToD3DCull(agiLastState.CullMode));

    // Depth writes, restored for the same reason as everything else here: the additive-glow branch
    // above may have switched them off, and FlushState() only re-issues D3DRS_ZWRITEENABLE when
    // agiCurState's value changes, so leaving it off would silently disable depth writes for every
    // following draw that happened not to toggle it.
    device->SetRenderState(D3DRS_ZWRITEENABLE, agiLastState.ZWrite ? TRUE : FALSE);

    // Blend mode is programmed above now, so it has to be put back too - agiLastState.BlendSet is
    // what FlushState() believes the device is in, and it only re-issues on a *change*, so leaving
    // this path's blend mode behind would silently apply it to every following CPU-path draw that
    // happens not to change blend set.
    //
    // Guarded because agiRendStateStruct::Reset() (agi/rsys.cpp) poisons the whole struct with
    // 0xFF, which is not a valid agiBlendSet - restoring blind would hit ApplyBlendSet()'s
    // "bad blend mode" Quitf if this path ever ran before the first full FlushState().
    if (agiLastState.BlendSet <= agiBlendSet::One_One)
        ApplyBlendSet(device, agiLastState.BlendSet);

    agiDX9TexDef* restore_tex = static_cast<agiDX9TexDef*>(agiLastState.Texture);
    IDirect3DTexture9* restore_handle = restore_tex ? restore_tex->GetHandle() : nullptr;

    device->SetTexture(0, restore_handle);
    current_texture_ = restore_handle;

    // Sampler filter *and address* state is global to stage 0, and agiDX9TexDef::SetFilters()
    // programs both from the texture it is called for (see the D3DTADDRESS note there). The
    // ApplyTexFilters() call above therefore left this draw's texture's wrap/clamp modes on the
    // device. Rebinding restore_handle does not undo that: FlushState() only calls
    // ApplyTexFilters() when it sees the bound texture *change*, and from its point of view nothing
    // changed - agiLastState.Texture still names the same texture it did before this call. So a
    // CLAMP-mode world texture (road and terrain sheets are clamped) silently left stage 0 in
    // CLAMP, and the next CPU-path draw of a WRAP texture tiled nothing, smearing its edge texels
    // across the surface. Re-apply the restored texture's own filters so the device matches what
    // the cache claims.
    if (restore_handle)
        ApplyTexFilters(restore_tex, agiLastState.TexFilter);

    ApplyTexEnv(device, tex_env_);

    // Per the D3D9 spec, XYZRHW (pretransformed) draws should completely ignore
    // D3DTS_WORLD/VIEW/PROJECTION - but some drivers (notably Intel's fixed-function-pipeline
    // implementations) have known quirks where leftover transform state affects supposedly
    // transform-immune draws (e.g. via internal clipping or fog referencing view-space Z). Reset
    // all three to identity so every subsequent pretransformed (agiScreenVtx) draw this frame is
    // guaranteed a clean slate, regardless of what this hardware-transform draw just set them to.
    D3DMATRIX identity {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;

    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetTransform(D3DTS_VIEW, &identity);

    // RTX Remix exception: do NOT reset PROJECTION to identity under the Remix bridge. Remix
    // classifies a fixed-function draw as UI when the current projection looks orthographic
    // (_44 == 1.0f, identity qualifies) and D3DRS_ZWRITEENABLE is off, and the first "UI" draw of
    // a frame latches RTX injection - every draw after it that frame is rasterized only, never
    // path traced (and the game still looks normal, because rtx.skipDrawCallsPostRTXInjection
    // defaults false). This engine draws its screen-space effects mid-scene - blob shadows, glow
    // cards, smoke - all RHW with zwrite off, so with an identity projection left behind the first
    // blob shadow of the frame quietly ended path tracing for everything after it. Leaving the
    // last perspective projection (_44 == 0) is free: RHW draws ignore all three transforms per
    // the D3D9 spec, and Remix's Vulkan fixed-function honours that. The genuine HUD still
    // composites because Present() injects RTX at end of frame regardless.
    if (!agiDX9RemixBridgeActive())
        device->SetTransform(D3DTS_PROJECTION, &identity);
}

// Harvests a world-space glow mesh as a light source. See agiworld/glowlight.h.
//
// The billboard path (agiMeshSet::DrawCard) misses everything vehicle-related: mmCarModel::DrawGlow
// and aiVehicleInstance::DrawGlow both submit a glow MESH, so head/tail/brake/reverse lights never
// reach it. This covers those, and any other AlphaGlow geometry in the city.
//
// Position and extent come from the submitted geometry rather than from the instance origin,
// because a tail light sits well off a car's centre and the whole point is that the light lands
// where the lamp is.
static void HarvestWorldGlow(
    agiDX9TexDef* texture, agiWorldVtx* vertices, u16* indices, i32 index_count, const Matrix34& world)
{
    if (!texture || (index_count <= 0))
        return;

    // One light PER FLARE, not one per glow mesh.
    //
    // A vehicle's glow mesh holds every lamp on one end of the car in a single submission - both
    // headlights, or both tail lights - so averaging the whole thing into a single centroid put one
    // light on the car's centre axis, between the actual lamps. That is exactly the reported "cars
    // have only one light right in the car's center axis; there should be two, right where their
    // alpha cards are".
    //
    // Each flare is a separate quad, so clustering triangles by proximity recovers them without
    // needing to know anything about how a particular car was modelled - it works for two
    // headlights, for a single centre brake light, or for the four-lamp arrangements some vehicles
    // use. The threshold is derived from the mesh's own extent rather than fixed in world units, so
    // it scales from a motorbike to a bus.
    constexpr i32 kMaxClusters = 4;

    struct Cluster
    {
        Vector3 Sum;
        Vector3 Min;
        Vector3 Max;
        f32 AccumR, AccumG, AccumB, AccumA;
        f32 AccumU, AccumV;
        i32 Count;
    };

    Cluster clusters[kMaxClusters] {};
    i32 cluster_count = 0;

    // Overall extent first, to size the clustering threshold.
    Vector3 bound_min = vertices[indices[0]].pos;
    Vector3 bound_max = bound_min;

    for (i32 i = 1; i < index_count; ++i)
    {
        const Vector3& p = vertices[indices[i]].pos;

        bound_min = {std::min(bound_min.x, p.x), std::min(bound_min.y, p.y), std::min(bound_min.z, p.z)};
        bound_max = {std::max(bound_max.x, p.x), std::max(bound_max.y, p.y), std::max(bound_max.z, p.z)};
    }

    const Vector3 extent = bound_max - bound_min;
    const f32 threshold = std::max(std::max({extent.x, extent.y, extent.z}) * 0.25f, 0.05f);
    const f32 threshold_sq = threshold * threshold;

    for (i32 tri = 0; (tri + 2) < index_count; tri += 3)
    {
        Vector3 centre =
            (vertices[indices[tri]].pos + vertices[indices[tri + 1]].pos + vertices[indices[tri + 2]].pos) / 3.0f;

        i32 slot = -1;

        for (i32 c = 0; c < cluster_count; ++c)
        {
            const Vector3 mean = clusters[c].Sum / static_cast<f32>(clusters[c].Count);

            if ((mean - centre).Mag2() <= threshold_sq)
            {
                slot = c;
                break;
            }
        }

        if (slot < 0)
        {
            if (cluster_count >= kMaxClusters)
                continue;

            slot = cluster_count++;
            clusters[slot] = {};
            clusters[slot].Min = centre;
            clusters[slot].Max = centre;
        }

        Cluster& cluster = clusters[slot];

        for (i32 k = 0; k < 3; ++k)
        {
            const agiWorldVtx& vtx = vertices[indices[tri + k]];

            cluster.Min = {std::min(cluster.Min.x, vtx.pos.x), std::min(cluster.Min.y, vtx.pos.y),
                std::min(cluster.Min.z, vtx.pos.z)};
            cluster.Max = {std::max(cluster.Max.x, vtx.pos.x), std::max(cluster.Max.y, vtx.pos.y),
                std::max(cluster.Max.z, vtx.pos.z)};

            cluster.AccumR += static_cast<f32>((vtx.color >> 16) & 0xFF);
            cluster.AccumG += static_cast<f32>((vtx.color >> 8) & 0xFF);
            cluster.AccumB += static_cast<f32>(vtx.color & 0xFF);
            cluster.AccumA += static_cast<f32>((vtx.color >> 24) & 0xFF);

            cluster.AccumU += vtx.tu;
            cluster.AccumV += vtx.tv;
        }

        cluster.Sum += centre;
        ++cluster.Count;
    }

    for (i32 c = 0; c < cluster_count; ++c)
    {
        const Cluster& cluster = clusters[c];

        const f32 inv_verts = 1.0f / static_cast<f32>(cluster.Count * 3);

        Vector3 local_centre = (cluster.Min + cluster.Max) * 0.5f;

        Vector3 world_centre;
        world_centre.Dot(local_centre, world);

        const Vector3 half = (cluster.Max - cluster.Min) * 0.5f;
        const f32 flare_size = std::max(std::max({half.x, half.y, half.z}), 0.05f);

        // The vertex colour is only the TINT. The hue comes from the glow texture itself, sampled at
        // the UVs this cluster reads - see agiDX9TexDef::SampleGlowColor.
        const f32 intensity = std::max((cluster.AccumA * inv_verts) / 255.0f, 1.0f / 255.0f);

        Vector3 tint {
            (cluster.AccumR * inv_verts / 255.0f) * intensity,
            (cluster.AccumG * inv_verts / 255.0f) * intensity,
            (cluster.AccumB * inv_verts / 255.0f) * intensity,
        };

        // agiGlowLightReach() is shared with the billboard route (agiAddGlowLight). The two used to
        // floor the reach differently - 14 here against 24 there - and since emitted intensity goes
        // with the square of reach, that alone made the same fixture ~3x brighter as a card than as
        // a mesh.
        agiAddGlowLightRGB(world_centre, tint, agiGlowLightReach(flare_size), texture, cluster.AccumU * inv_verts,
            cluster.AccumV * inv_verts);
    }
}

bool agiDX9Rasterizer::MeshWorld(agiWorldVtx* vertices, i32 vertex_count, u16* indices, i32 index_count,
    const Matrix34& world, const Matrix34& view, const agiViewParameters& proj_params, bool static_lighting,
    const agiNativeMaterialFx* fx, bool hardware_lighting)
{
    if (!IsAppActive() || (vertex_count == 0) || (index_count == 0))
        return true;

    FlushState();

    ARTS_UTIMED(agiRasterization);
    ++STATS.GeomCalls;

    ++agiDX9Census.WorldCalls;
    agiDX9Census.WorldTris += index_count / 3;

    if (!hardware_lighting)
        agiDX9Census.WorldUnlitTris += index_count / 3;
    else if (static_lighting)
        agiDX9Census.WorldStaticLitTris += index_count / 3;

    IDirect3DDevice9* device = Pipe()->Context()->GetDevice();

    // Depth bias, now off by default - see docs/dx9_rendering_pathways.md, A4.
    //
    // This existed to win one specific fight: wheel/tyre geometry sitting millimetres above the
    // road was losing the depth test against the road, because the road was still drawn by the CPU
    // pretransform path. The two paths compute the same depth by different routes - the CPU as a
    // scalar chain (view_z * ProjZZ + ProjZW) * inv_w * DepthScale + DepthOffset, this path via the
    // GPU's perspective divide of a matrix-transformed clip Z - and they agree mathematically but
    // not bit-for-bit, which is enough to flip the winner for two coplanar surfaces.
    //
    // That premise no longer holds. DrawLitEnv() routes the road through this same path now, so
    // wheels and road are both hardware-transformed and compute their depth identically; there is
    // no cross-path comparison left to lose. What remains is a constant pull toward the camera
    // applied to *all* world geometry, biasing it against everything still on the CPU path - blob
    // shadows, tyre smoke, glows and decals, which sit close to surfaces by design and are exactly
    // the content a global bias makes fight.
    //
    // Kept as a parameter rather than deleted because the census still reports a nonzero in-scene
    // screen-triangle count, so some 3D content has not moved over yet.
    const f32 depth_bias = PARAM_d3d9_depthbias.get_or(0.0f);
    device->SetRenderState(D3DRS_DEPTHBIAS, *reinterpret_cast<const DWORD*>(&depth_bias));

    // FlushState()'s texture/texture-stage bookkeeping (current_texture_/tex_env_) only updates
    // when agiCurState::DrawMode == agiDrawTextured - a piece of global state set deep inside the
    // still-closed CPU FirstPass()/material-bind code, which this native-transform path never
    // establishes since it bypasses the CPU pretransform pipeline entirely. Relying on that
    // leftover state here silently no-op'd every draw whenever the last thing drawn earlier in
    // the frame happened to leave DrawMode/tex_env_ in the "wrong" shape (confirmed via testing:
    // vehicle bodies - player and AI traffic - rendered completely invisible while still
    // reporting success, because current_texture_ had been zeroed by unrelated leftover state).
    // Bind the texture and its stage ops directly from agiCurState here instead, independent of
    // the pretransform path's DrawMode-gated bookkeeping.
    agiDX9TexDef* native_tex = static_cast<agiDX9TexDef*>(agiCurState.GetTexture());
    IDirect3DTexture9* native_handle = native_tex ? native_tex->GetHandle() : nullptr;

    device->SetTexture(0, native_handle);

    if (native_handle)
    {
        // FlushState() only applies these when it sees the bound texture *change*, and it is
        // gated on DrawMode == agiDrawTextured, which this path never establishes - so without
        // this the native path inherited whatever filters/addressing the last CPU-path draw left,
        // including the wrong wrap/clamp mode for this texture.
        ApplyTexFilters(native_tex, agiCurState.GetTexFilter());

        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    }
    else
    {
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    }

    current_texture_ = native_handle;

    // Alpha state has to be derived here too, for the same reason the texture bind above does.
    // FlushState() computes it as
    //     alpha_enable = agiCurState.GetAlphaEnable()
    //                  | texture->Tex.HasAlpha() | texture->Tex.UseChromakey()
    // but its `texture` is only non-null when agiCurState::DrawMode == agiDrawTextured - a mode
    // this path never establishes, since it bypasses the CPU pretransform pipeline that sets it.
    // So FlushState() saw no texture, never ORed in the per-texture terms, and left
    // ALPHABLENDENABLE/ALPHATESTENABLE off for exactly the textures that need them: every
    // alpha-keyed texture (light flares, coronas, tree and foliage billboards, fences, railings)
    // rasterised its transparent texels as opaque, which is the field of solid black quads that
    // showed up across the city once this path started carrying most of the scene.
    bool alpha_enable = agiCurState.GetAlphaEnable();

    if (native_tex)
        alpha_enable |= native_tex->Tex.HasAlpha() || native_tex->Tex.UseChromakey();

    // AlphaGlow textures (light flares, coronas, headlight/taillight glows, the fake headlight
    // cones) are the case the two terms above deliberately do NOT cover. agiTexProp::AlphaGlow
    // content is meant to be composited *additively* - the texture's black background contributes
    // nothing under One_One - so both getmesh.cpp (Tex.Flags &= ~Alpha when the renderer reports
    // AdditiveBlending, which agisdl/sdlsetup.cpp always does) and agiDX9TexDef::BeginGfx() strip
    // the Alpha flag from them at load. HasAlpha()/UseChromakey() are therefore both false, so
    // without this term alpha_enable stays false, ALPHABLENDENABLE goes off, and the glow's black
    // background rasterises opaque - a solid black box with the glow inside it.
    //
    // On the CPU path this never arises, because the draw is issued by agiTexSorter::DoTexture()
    // (closed assembly, game.asm), which binds blend state per texture from these same flags.
    // This path bypasses the sorter entirely, so it has to do that binding itself - the same class
    // of omission already fixed above for the texture bind and below for the blend mode.
    bool additive_glow = native_tex && (native_tex->Tex.Props & agiTexProp::AlphaGlow);

    u8 alpha_ref = agiCurState.GetAlphaRef();

    // Alpha *blending* and alpha *testing* are deliberately decoupled for the glow case. Blending
    // is what makes the black background vanish under One_One and must be on. Testing must NOT be
    // turned on for it: the glow was rebuilt as an X8R8G8B8 texture when its Alpha flag was
    // stripped, so its sampled alpha is a constant 1.0 and the stage modulates that by the vertex
    // diffuse alpha - which for a fading glow is small. A GREATER/AlphaRef test against that would
    // discard exactly the faint pixels the additive blend exists to draw.
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, (alpha_enable || additive_glow) ? TRUE : FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, alpha_enable ? TRUE : FALSE);

    if (alpha_enable)
    {
        device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        device->SetRenderState(D3DRS_ALPHAREF, alpha_ref);
    }

    // Additive glows must not write depth.
    //
    // A glow quad is a transparent overlay: its black background contributes nothing to colour under
    // One_One, but if it writes Z it still stakes a claim on those pixels. Anything drawn afterwards
    // and behind it - snow, rain, tyre smoke, other glows - is then depth-rejected across the whole
    // quad, so the particle field develops a rectangular hole exactly the size of the sprite. That
    // is the reported "brake lights and headlights cull the particles near them, so the sharp
    // corners become visible": the corners being seen are the glow quad's own bounds, punched out
    // of the particles behind it.
    //
    // The CPU path never hits this because its draws go through agiTexSorter, which binds depth
    // state per texture from the same flags. This path bypasses the sorter, so it has to do it here
    // - the same class of omission already fixed for the texture bind, the blend mode and fog.
    // Restored by RestoreStateAfterWorldDraw() from agiLastState.
    if (additive_glow)
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

    // Harvest this glow as a light source, but only for real 3D content - IsInScene() keeps menu
    // and showroom glows (which have no city around them to light) out of the set.
    if (additive_glow && Pipe()->IsInScene())
        HarvestWorldGlow(native_tex, vertices, indices, index_count, world);

    // Fog off for additive glows on this path as well - same reason as the screen path, see
    // IsAdditiveGlow(). Restored by RestoreStateAfterWorldDraw().
    if (additive_glow)
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);

    // Program the blend mode rather than inheriting whatever the last CPU-path draw left on the
    // device. An AlphaGlow texture is additive by definition and has no usable alpha channel left
    // to blend with (see above), so it gets One_One regardless of what agiCurState currently says.
    ApplyBlendSet(device, additive_glow ? agiBlendSet::One_One : agiCurState.GetBlendSet());

    // Hardware backface culling, exactly as the CPU path gets it. This used to be D3DCULL_NONE, on
    // the reasoning that DrawNativeTransform() had already excluded backfacing facets on the CPU
    // via plane equations so no winding convention was available. Both halves of that are wrong.
    //
    // The CPU plane test is agiMeshSet::IsBackfacing(), and it opens with
    //     return AllowEyeBackfacing && ...
    // AllowEyeBackfacing (agiworld/meshrend.cpp) initialises to *false* and is only raised by
    // agiMeshSet::InitMtx (game.asm ~324504), which does so only when all three of: the caller
    // passed planes && SurfaceCount > 1; MirrorMode is clear; and the transform passes InitMtx's
    // scale tolerance check. It is set straight back to 0 on every other path (~324569). So for a
    // large share of draws IsBackfacing() returns false for every facet and the "already culled on
    // the CPU" premise simply does not hold - nothing was culled.
    //
    // That was survivable on the CPU path because it never relied on the plane test alone: it feeds
    // its screen-space triangles through FlushState(), which programs D3DRS_CULLMODE from
    // agiCurState - agiRasterizer's constructor sets agiCullMode::CCW (agi/rsys.cpp) - so the GPU
    // culled by winding as a second line of defence. Forcing D3DCULL_NONE here removed that second
    // line and left the world path drawing every back face in the scene. That is the direct cause
    // of the reported artifacts: interior/rear building walls rasterising over their own front
    // faces (the "black triangle spots" and the "overlapping UV" look, which is a back face showing
    // its texture mirrored), and coplanar road surfaces fighting their own reverse side (the road
    // z-fighting) - none of which the CPU path exhibits, because it culls them.
    //
    // The winding is the same on both paths. The facet-to-triangle order here matches ClipTri()'s
    // exactly (quads split 1,2,3 / 1,3,0), and InitViewport() maps NDC to screen with
    // HalfWidth = +w/2 and HalfHeight = -h/2 - the same X-positive, Y-flipped mapping D3D9's own
    // viewport transform applies to our projected output. Same triangles, same screen-space
    // orientation, so the same cull mode is correct.
    //
    // Taking it from agiCurState rather than hardcoding CCW keeps the two paths in lockstep through
    // the places that deliberately disable culling for two-sided geometry (agiMeshSet::DrawWideLines
    // and mmShard::Draw both set agiCullMode::None around their draws).
    // Flipped - see ToD3DCullFlipped(). agiMeshSet::FlipX needs no extra compensation here even
    // though BuildProjectionMatrix() negates _11 for it: the CPU path mirrors the same way (by
    // negating InitViewport's HalfWidth) and likewise leaves its cull mode alone, so both paths
    // reverse together and stay in agreement.
    device->SetRenderState(D3DRS_CULLMODE, ToD3DCullFlipped(agiCurState.GetCullMode()));

    // D3D9 fixed-function lighting does not renormalise normals after the world/view transform, and
    // agiWorldVtx::normal arrives as a unit vector from UnpackNormal[] in *model* space. Any
    // instance whose world matrix carries scale therefore hands the lighting pipeline a normal
    // whose length is no longer 1, and D3D9's N.L falls straight out of it - so the surface reads
    // uniformly too bright or too dark depending on the scale factor, and the specular term (which
    // is driven by a power of that dot product) exaggerates it further. Scaled instances are known
    // to exist here: InitMtx's guard for AllowEyeBackfacing is precisely a scale-tolerance test on
    // the current transform, which would be pointless if every transform were rigid.
    device->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);

    // Vertex fog does not exist on this path and has to be re-expressed as table fog.
    //
    // agiFogMode::Vertex means "the fog factor is already baked into each vertex's specular alpha"
    // - FirstPass() computes it on the CPU from agiMeshSet::FogValue, and FlushState() programs
    // FOGTABLEMODE = FOGVERTEXMODE = NONE so D3D9 reads that alpha directly. But this path's FVF is
    // D3DFVF_XYZ|NORMAL|DIFFUSE|TEX1: agiWorldVtx carries no specular component at all, so D3D9
    // reads a fog factor of zero for every vertex and renders the entire submission in flat fog
    // colour. Whole city blocks turn into featureless sky-coloured silhouettes.
    //
    // This is not the default configuration - UsePixelFog is derived from dxiRendererInfo_t's
    // SpecialFlags bit 0x10 (game.asm ~177811), which agisdl/sdlsetup.cpp now sets for D3D9, so
    // mmCullCity::Cull() normally selects agiFogMode::Pixel. It is reachable though: UsePixelFog is
    // a registered debug-menu tweakable, and it is what every non-pixel-fog renderer uses.
    //
    // Ask the GPU for the same curve instead. agiMeshSet::SetFog() stores FogValue = 255/fog_end,
    // so fog_end recovers exactly, and D3D9 linear table fog over view-space depth reproduces the
    // CPU's per-vertex ramp (better, in fact - it interpolates per pixel).
    const bool remap_vertex_fog = (agiLastState.FogMode == agiFogMode::Vertex);

    if (remap_vertex_fog)
    {
        if (agiMeshSet::FogValue > 0.0f)
        {
            constexpr f32 kFogStart = 1.0f;
            const f32 fog_end = 255.0f / agiMeshSet::FogValue;

            device->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
            device->SetRenderState(D3DRS_FOGSTART, *reinterpret_cast<const DWORD*>(&kFogStart));
            device->SetRenderState(D3DRS_FOGEND, *reinterpret_cast<const DWORD*>(&fog_end));
        }
        else
        {
            // No usable range to rebuild the ramp from - drawing unfogged is a far smaller error
            // than drawing in solid fog colour.
            device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        }
    }

    D3DMATRIX world_mat = ToD3DMatrix(world);

    // The CPU pretransform path (agiMeshSet::ARTS_TRANSFORM_DOT, meshrend.cpp) doesn't feed the
    // engine's View matrix to anything hardware-facing - it only ever uses the shared, closed-
    // Init()-refreshed `M` matrix, which was confirmed (by direct comparison against
    // Dot(World, View)) to negate the Z contribution of View relative to this raw Matrix34: M's
    // Z-row is exactly -1 times combined(World, View)'s Z-row, everywhere. That negation converts
    // this engine's view-space convention (camera looks down -Z) into the positive-forward
    // convention agiMeshSet::ToScreen()'s perspective divide (and every renderer built on it,
    // including OpenGL) expects. Feeding `view` to SetTransform(D3DTS_VIEW) unmodified hands D3D9's
    // hardware transform pipeline the *unflipped* convention, so its own view-space Z comes out
    // with the opposite sign from what the rest of the engine (and this same draw's projection
    // math below, tuned to match the CPU path) assumes - corrupting depth-adjacent geometry while
    // still submitting successfully (no error, just wrong). Negate View's Z-row here to match.
    Matrix34 view_zflip = view;
    view_zflip.m0.z = -view_zflip.m0.z;
    view_zflip.m1.z = -view_zflip.m1.z;
    view_zflip.m2.z = -view_zflip.m2.z;
    view_zflip.m3.z = -view_zflip.m3.z;

    // ---------------------------------------------------------------------------------------
    // Pathway B fork.
    //
    // Everything above this point is shared: texture bind, alpha/blend/cull/depth are output-merger
    // and sampler state that a pixel shader does not replace. Only the transform-and-light half of
    // the pipeline differs, so that is all that forks here.
    // ---------------------------------------------------------------------------------------------
    agiDX9WorldShader* shader = Pipe()->WorldShader();

    if (shader)
    {
        agiDX9WorldDrawInfo info {};
        info.World = &world;
        info.ViewZFlip = &view_zflip;
        info.Proj = &proj_params;
        info.Texture = native_tex;
        info.Lit = hardware_lighting;
        info.StaticLighting = static_lighting;

        shader->Setup(device, info);

        device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vertex_count, index_count / 3, indices, D3DFMT_INDEX16,
            vertices, sizeof(agiWorldVtx));

        // Back to fixed function before anything else touches the device. The reflection pass below
        // and every pretransformed draw after this point are FF, and a left-over vertex shader
        // would reinterpret their XYZRHW vertices as model-space positions.
        shader->Unbind(device);

        if (fx && fx->ReflectionTexture)
        {
            DrawVehicleReflectionPass(device, vertices, vertex_count, indices, index_count, world, view, *fx);
            current_texture_ = nullptr;
        }

        RestoreStateAfterWorldDraw(remap_vertex_fog);
        return true;
    }

    D3DMATRIX view_mat = ToD3DMatrix(view_zflip);
    D3DMATRIX proj_mat = BuildProjectionMatrix(proj_params);

    device->SetTransform(D3DTS_WORLD, &world_mat);
    device->SetTransform(D3DTS_VIEW, &view_mat);
    device->SetTransform(D3DTS_PROJECTION, &proj_mat);

    // Meshes loaded without MESH_SET_NORMAL carry no normals, so there is nothing for hardware
    // lighting to work from - their agiWorldVtx::normal is filler. The CPU path draws them
    // straight from their baked vertex colors, and this reproduces that exactly: lighting off, so
    // D3D9 takes vertex diffuse through unmodified. Only the transform moves to hardware, which is
    // the whole point - it puts this geometry (most static city scenery, and the low-detail LODs
    // that make up the far half of the view) into the world-space stream RTX Remix can use,
    // without changing a single pixel of how it is shaded.
    if (!hardware_lighting)
    {
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    }
    else
    {
        device->SetRenderState(D3DRS_LIGHTING, TRUE);

        // Specular is legitimate for the dynamic rig - SetupD3D9Material() takes its specular
        // colour and power from the engine's own agiMtlDef (agiCurState.GetMtl()), i.e. real
        // authored material data for cars and movers. The static city rig has no specular concept
        // at all, so it only gets one when explicitly asked for. See SetupD3D9StaticMaterial().
        device->SetRenderState(D3DRS_SPECULARENABLE, (!static_lighting || agiDX9WantsStaticSpecular()) ? TRUE : FALSE);

        device->SetRenderState(D3DRS_COLORVERTEX, TRUE);
        device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);

        // Ambient must come from the vertex colour, not the material. agiMeshLighterTriple
        // (agiworld/meshlight.cpp, mmxTriple) computes
        //     intensity = key + fill1 + fill2 + ambient      (clamped to 1.0)
        //     output    = vertex_colour * intensity
        // i.e. ambient is *inside* the term the vertex colour multiplies. With D3DMCS_MATERIAL and
        // a material Ambient of (1,1,1,1), D3D9 instead computed
        //     output = vertex_colour * (key + fill1 + fill2) + ambient
        // adding the full, unmodulated ambient on top of every surface. That washes out exactly the
        // content whose baked vertex colours are dark - night-time building facades and the
        // ambient-occluded underside of geometry - and it is a flat additive lift, so it cannot be
        // dialled out by the ambient value alone. D3DMCS_COLOR1 puts ambient back inside the
        // product and matches the CPU rig term for term.
        device->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1);
        device->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
        device->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    }

    if (!hardware_lighting)
    {
        // No light/material setup at all - nothing consumes it with lighting disabled.
    }
    else if (static_lighting)
    {
        const Vector3& ambient = agiMeshLighterAmbient;
        device->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_COLORVALUE(ambient.x, ambient.y, ambient.z, 1.0f));

        SetupD3D9StaticLights(device);
        SetupD3D9StaticMaterial(device);
    }
    else
    {
        const Vector3& scene_ambient = agiLighter::SceneAmbient;
        device->SetRenderState(
            D3DRS_AMBIENT, D3DCOLOR_COLORVALUE(scene_ambient.x, scene_ambient.y, scene_ambient.z, 1.0f));

        SetupD3D9Lights(device);
        SetupD3D9Material(device);
    }

    device->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);

    i32 primitive_count = index_count / 3;

    device->DrawIndexedPrimitiveUP(
        D3DPT_TRIANGLELIST, 0, vertex_count, primitive_count, indices, D3DFMT_INDEX16, vertices, sizeof(agiWorldVtx));

    if (fx && fx->ReflectionTexture)
    {
        DrawVehicleReflectionPass(device, vertices, vertex_count, indices, index_count, world, view, *fx);
        current_texture_ = nullptr;
    }

    RestoreStateAfterWorldDraw(remap_vertex_fog);

    return true;
}

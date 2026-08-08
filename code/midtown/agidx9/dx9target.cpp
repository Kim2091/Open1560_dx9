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

#include "dx9target.h"

#include "dx9context.h"

#include "dx9_windows.h"

define_dummy_symbol(agidx9_dx9target);

// --- Default-pool registry ---------------------------------------------------------------------

static agiDX9DefaultPoolResource* g_DefaultPoolHead = nullptr;

agiDX9DefaultPoolResource::agiDX9DefaultPoolResource()
{
    next_ = g_DefaultPoolHead;

    if (next_)
        next_->prev_ = this;

    g_DefaultPoolHead = this;
}

agiDX9DefaultPoolResource::~agiDX9DefaultPoolResource()
{
    if (prev_)
        prev_->next_ = next_;
    else if (g_DefaultPoolHead == this)
        g_DefaultPoolHead = next_;

    if (next_)
        next_->prev_ = prev_;

    next_ = nullptr;
    prev_ = nullptr;
}

bool agiDX9HasDefaultPoolResources()
{
    return g_DefaultPoolHead != nullptr;
}

void agiDX9ReleaseDefaultPoolResources()
{
    for (agiDX9DefaultPoolResource* i = g_DefaultPoolHead; i; i = i->next_)
        i->OnDeviceLost();
}

bool agiDX9RestoreDefaultPoolResources(IDirect3DDevice9* device)
{
    bool all_restored = true;

    for (agiDX9DefaultPoolResource* i = g_DefaultPoolHead; i; i = i->next_)
    {
        if (!i->OnDeviceReset(device))
            all_restored = false;
    }

    return all_restored;
}

// --- Format selection --------------------------------------------------------------------------

// Preferred first. A16B16G16R16F is the point of the exercise: the scene routinely sums past 1.0
// once the clustered lights are in it, and an 8-bit target throws that away before bloom or a
// tonemap can do anything with it. A32B32G32R32F is not in the list - it doubles the bandwidth for
// precision nothing here needs, and many parts cannot filter it.
static D3DFORMAT PickTargetFormat(IDirect3D9* d3d, D3DFORMAT adapter_format, D3DFORMAT requested)
{
    static const D3DFORMAT kHdrCandidates[] {
        D3DFMT_A16B16G16R16F,
        D3DFMT_A2B10G10R10,
        D3DFMT_A8R8G8B8,
    };

    const D3DFORMAT* candidates = kHdrCandidates;
    u32 count = static_cast<u32>(std::size(kHdrCandidates));

    D3DFORMAT single[2] {};

    if (requested != D3DFMT_UNKNOWN)
    {
        // An explicit request still falls back to the backbuffer format rather than failing
        // outright - a shadow map that is R32F on one part and A8R8G8B8 on another is worth having.
        single[0] = requested;
        single[1] = D3DFMT_A8R8G8B8;
        candidates = single;
        count = 2;
    }

    for (u32 i = 0; i < count; ++i)
    {
        if (SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, adapter_format,
                D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, candidates[i])))
        {
            return candidates[i];
        }
    }

    return D3DFMT_UNKNOWN;
}

// --- agiDX9RenderTarget ------------------------------------------------------------------------

agiDX9RenderTarget::~agiDX9RenderTarget()
{
    Shutdown();
}

bool agiDX9RenderTarget::Init(IDirect3DDevice9* device, u32 width, u32 height, u32 format, bool own_depth)
{
    Shutdown();

    if (!device || (width == 0) || (height == 0))
        return false;

    device_ = device;
    width_ = width;
    height_ = height;
    own_depth_ = own_depth;

    IDirect3D9* d3d = nullptr;

    if (FAILED(device->GetDirect3D(&d3d)) || !d3d)
        return false;

    D3DDISPLAYMODE mode {};
    d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode);

    const D3DFORMAT picked = PickTargetFormat(d3d, mode.Format, static_cast<D3DFORMAT>(format));

    d3d->Release();

    if (picked == D3DFMT_UNKNOWN)
    {
        Warningf("DX9 RT: no usable render-target format for %ux%u", width, height);
        return false;
    }

    format_ = static_cast<u32>(picked);

    if (!Create(device))
    {
        Shutdown();
        return false;
    }

    Displayf("DX9 RT: %ux%u format 0x%X%s", width_, height_, format_, own_depth_ ? " (own depth)" : "");

    return true;
}

bool agiDX9RenderTarget::Create(IDirect3DDevice9* device)
{
    // D3DPOOL_DEFAULT is mandatory for a render target - it is the one thing that makes this class
    // need agiDX9DefaultPoolResource at all.
    HRESULT hr = device->CreateTexture(width_, height_, 1, D3DUSAGE_RENDERTARGET, static_cast<D3DFORMAT>(format_),
        D3DPOOL_DEFAULT, &texture_, nullptr);

    if (FAILED(hr))
    {
        Warningf("DX9 RT: CreateTexture failed, code %x", static_cast<u32>(hr));
        return false;
    }

    hr = texture_->GetSurfaceLevel(0, &surface_);

    if (FAILED(hr))
    {
        Warningf("DX9 RT: GetSurfaceLevel failed, code %x", static_cast<u32>(hr));
        return false;
    }

    if (own_depth_)
    {
        // Matched to the device's own depth format so the pair is guaranteed compatible.
        D3DSURFACE_DESC desc {};
        IDirect3DSurface9* device_depth = nullptr;

        D3DFORMAT depth_format = D3DFMT_D24S8;

        if (SUCCEEDED(device->GetDepthStencilSurface(&device_depth)) && device_depth)
        {
            if (SUCCEEDED(device_depth->GetDesc(&desc)))
                depth_format = desc.Format;

            device_depth->Release();
        }

        hr = device->CreateDepthStencilSurface(
            width_, height_, depth_format, D3DMULTISAMPLE_NONE, 0, TRUE, &depth_, nullptr);

        if (FAILED(hr))
        {
            Warningf("DX9 RT: CreateDepthStencilSurface failed, code %x", static_cast<u32>(hr));
            return false;
        }
    }

    return true;
}

void agiDX9RenderTarget::Shutdown()
{
    // Never leave a released surface bound to the device.
    if (in_scope_ && device_)
        End(device_);

    if (depth_)
    {
        depth_->Release();
        depth_ = nullptr;
    }

    if (surface_)
    {
        surface_->Release();
        surface_ = nullptr;
    }

    if (texture_)
    {
        texture_->Release();
        texture_ = nullptr;
    }
}

void agiDX9RenderTarget::OnDeviceLost()
{
    // Keep width/height/format - OnDeviceReset rebuilds from them.
    Shutdown();
}

bool agiDX9RenderTarget::OnDeviceReset(IDirect3DDevice9* device)
{
    if ((width_ == 0) || (height_ == 0))
        return true; // never initialised; nothing to restore

    device_ = device;

    if (!Create(device))
    {
        Shutdown();
        return false;
    }

    return true;
}

bool agiDX9RenderTarget::Begin(IDirect3DDevice9* device)
{
    if (!surface_ || in_scope_)
        return false;

    if (FAILED(device->GetRenderTarget(0, &saved_target_)))
        return false;

    // The auto depth-stencil may legitimately be absent, so a failure here is not fatal - it just
    // means there is nothing to put back.
    if (FAILED(device->GetDepthStencilSurface(&saved_depth_)))
        saved_depth_ = nullptr;

    if (FAILED(device->SetRenderTarget(0, surface_)))
    {
        if (saved_target_)
        {
            saved_target_->Release();
            saved_target_ = nullptr;
        }

        if (saved_depth_)
        {
            saved_depth_->Release();
            saved_depth_ = nullptr;
        }

        return false;
    }

    if (own_depth_)
        device->SetDepthStencilSurface(depth_);

    // SetRenderTarget resets the viewport to the full surface, but say so explicitly rather than
    // relying on it - agiDX9Viewport::Activate leaves its own rectangle on the device, and the
    // rear-view mirror's is a quarter of the screen.
    D3DVIEWPORT9 viewport {};
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = width_;
    viewport.Height = height_;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;

    device->SetViewport(&viewport);

    in_scope_ = true;

    return true;
}

void agiDX9RenderTarget::End(IDirect3DDevice9* device)
{
    if (!in_scope_)
        return;

    in_scope_ = false;

    if (saved_target_)
    {
        device->SetRenderTarget(0, saved_target_);
        saved_target_->Release();
        saved_target_ = nullptr;
    }

    if (saved_depth_)
    {
        device->SetDepthStencilSurface(saved_depth_);
        saved_depth_->Release();
        saved_depth_ = nullptr;
    }
}

// --- Fullscreen blit ---------------------------------------------------------------------------

namespace
{
    struct BlitVertex
    {
        f32 x, y, z, rhw;
        f32 u, v;
    };
} // namespace

void agiDX9BlitFullscreen(IDirect3DDevice9* device, IDirect3DTexture9* texture, u32 width, u32 height)
{
    if (!device || !texture)
        return;

    const f32 w = static_cast<f32>(width);
    const f32 h = static_cast<f32>(height);

    // -0.5 on position, not +0.5 on the UVs: it puts the pixel centres on the texel centres for a
    // 1:1 copy. See the note in the header.
    const BlitVertex quad[4] {
        {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
        {w - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
        {-0.5f, h - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f},
        {w - 0.5f, h - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f},
    };

    device->SetTexture(0, texture);

    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);

    // POINT and CLAMP: this is a 1:1 copy, so any filtering can only soften it.
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(BlitVertex));

    device->SetTexture(0, nullptr);

    // agiLastState is stale after this - every draw here went straight to the device, which is the
    // same hazard documented for MeshWorld. The caller is responsible for the invalidation; see
    // agiDX9Pipeline::EndFrame.
}

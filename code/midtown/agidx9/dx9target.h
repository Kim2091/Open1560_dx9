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

struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;

// Render-to-texture for the programmable path. This is the infrastructure
// docs/handoff_dx9_renderer.md §6 calls the highest-leverage missing piece: shadow maps,
// post-processing (bloom, a real tonemap pass, antialiasing) and environment probes all need
// somewhere to render that is not the backbuffer, and none of them can start without it.
//
// NOTE ON RTX REMIX. Anything that composites through one of these is Pathway B only, and is
// fundamentally invisible to Remix - Remix reconstructs a scene from the draws it sees and does its
// own rendering, so a post-processed image blitted over the top is at best ignored and at worst
// mistaken for UI. That is not a defect to fix later; it is the same divide as the two pathways
// (see dx9_rendering_pathways.md §0). Pathway A must keep drawing to the real backbuffer.

// Anything holding D3DPOOL_DEFAULT allocations must derive from this.
//
// D3DPOOL_DEFAULT is the whole reason this class exists. Every other resource in this backend is
// D3DPOOL_MANAGED and survives a device reset untouched, which is why agiDX9Context::ResetDevice()
// could get away with calling Reset() and nothing else. A default-pool resource cannot: D3D9 fails
// Reset with D3DERR_INVALIDCALL while even one is alive.
//
// That failure is not cosmetic here. Reset is on the path the menu <-> gameplay resolution change
// takes (agiDX9Context adopting the parked device, see dx9pipe.cpp), and the fallback when it fails
// is to destroy the device and build a new one - precisely the thing that breaks the RTX Remix
// bridge and that the parked device exists to avoid. So adding render targets without this would
// have reintroduced a bug that was already found and fixed the hard way.
//
// Instances register themselves on construction and unregister on destruction, so the context can
// release and restore all of them around a Reset without knowing what they are.
class agiDX9DefaultPoolResource;

void agiDX9ReleaseDefaultPoolResources();
bool agiDX9RestoreDefaultPoolResources(IDirect3DDevice9* device);

class agiDX9DefaultPoolResource
{
public:
    agiDX9DefaultPoolResource();
    virtual ~agiDX9DefaultPoolResource();

    ARTS_NON_COPYABLE(agiDX9DefaultPoolResource);

    // Release every D3DPOOL_DEFAULT allocation. Must leave the object safe to destroy.
    virtual void OnDeviceLost() = 0;

    // Recreate what OnDeviceLost() released. False means the object could not be restored; the
    // caller decides whether that is fatal.
    virtual bool OnDeviceReset(IDirect3DDevice9* device) = 0;

private:
    agiDX9DefaultPoolResource* next_ {};
    agiDX9DefaultPoolResource* prev_ {};

    // The registry walkers are the only things that may traverse the list.
    friend void agiDX9ReleaseDefaultPoolResources();
    friend bool agiDX9RestoreDefaultPoolResources(IDirect3DDevice9* device);
};

// agiDX9ReleaseDefaultPoolResources / agiDX9RestoreDefaultPoolResources are declared above the
// class so they can be friends of it. Call them around IDirect3DDevice9::Reset - see
// agiDX9Context::ResetDevice, the only caller.

// True while any default-pool resource is registered. Lets the reset path report the real reason a
// Reset failed instead of guessing.
bool agiDX9HasDefaultPoolResources();

class agiDX9RenderTarget final : public agiDX9DefaultPoolResource
{
public:
    agiDX9RenderTarget() = default;
    ~agiDX9RenderTarget() override;

    // `format` is a D3DFORMAT. Pass 0 for "pick an HDR format if one is available, otherwise the
    // backbuffer format" - which is what post-processing wants, because the game is full of
    // emissive glows that currently clamp at 1.0 with nowhere to put the excess.
    //
    // `own_depth` creates a matching depth-stencil surface. A full-screen target does not need one:
    // the device's auto depth-stencil is already the right size and can be shared. A shadow map,
    // which is smaller than the backbuffer, does.
    bool Init(IDirect3DDevice9* device, u32 width, u32 height, u32 format = 0, bool own_depth = false);
    void Shutdown();

    bool IsValid() const
    {
        return texture_ != nullptr;
    }

    // Binds this target and its viewport, remembering what was bound before. Nested Begin() calls
    // are not supported - one level, restored by End().
    bool Begin(IDirect3DDevice9* device);

    // Restores whatever was bound when Begin() was called.
    void End(IDirect3DDevice9* device);

    IDirect3DTexture9* GetTexture() const
    {
        return texture_;
    }

    u32 GetWidth() const
    {
        return width_;
    }

    u32 GetHeight() const
    {
        return height_;
    }

    u32 GetFormat() const
    {
        return format_;
    }

    void OnDeviceLost() override;
    bool OnDeviceReset(IDirect3DDevice9* device) override;

private:
    bool Create(IDirect3DDevice9* device);

    IDirect3DTexture9* texture_ {};
    IDirect3DSurface9* surface_ {};
    IDirect3DSurface9* depth_ {};

    // Saved across Begin()/End().
    IDirect3DSurface9* saved_target_ {};
    IDirect3DSurface9* saved_depth_ {};

    IDirect3DDevice9* device_ {}; // borrowed, for OnDeviceReset

    u32 width_ {};
    u32 height_ {};
    u32 format_ {}; // D3DFORMAT
    bool own_depth_ {};
    bool in_scope_ {};
};

// Draws `texture` over the whole current render target as a single quad.
//
// Fixed function and pretransformed on purpose: a straight copy needs no shader, and this has to
// work even when the programmable path failed to come up. Callers that want a shader (bloom,
// tonemap, FXAA) bind one before calling and unbind after - the quad itself does not care.
//
// The half-texel offset is applied here. D3D9 samples texels at their centres but rasterises pixels
// from their corners, so a full-screen blit without it is half a pixel soft in both axes, which
// looks exactly like a resolution mismatch and is the classic way to spend an afternoon.
void agiDX9BlitFullscreen(IDirect3DDevice9* device, IDirect3DTexture9* texture, u32 width, u32 height);

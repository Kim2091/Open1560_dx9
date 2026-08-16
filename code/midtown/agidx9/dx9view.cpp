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

#include "dx9view.h"

#include "agi/error.h"
#include "agi/pipeline.h"
#include "agi/rsys.h"
#include "data7/utimer.h"

#include "dx9context.h"
#include "dx9rsys.h"

#include "dx9_windows.h"

#include <algorithm>

define_dummy_symbol(agidx9_dx9view);

void agiDX9Viewport::EndGfx()
{
    state_ = 0;
}

i32 agiDX9Viewport::BeginGfx()
{
    if (Pipe()->HaveGfxStarted())
    {
        if (state_)
            return AGI_ERROR_ALREADY_INITIALIZED;

        state_ = 1;
    }

    return AGI_ERROR_SUCCESS;
}

// agiViewParameters::X/Y/Width/Height are fractions of the pipeline resolution, with Y measured
// from the *bottom* - the OpenGL convention the engine's viewport handling is built around
// (agigl/glview.cpp feeds Y straight to glScissor, whose origin is bottom-left, and
// agiMeshSet::InitViewport converts it with `y = pipe_height - (y + h)` to produce the top-left
// origin agiScreenVtx uses). D3D9 is top-left throughout, so the same conversion is needed here.
void agiDX9Viewport::GetPixelRect(i32& x, i32& y, i32& w, i32& h) const
{
    i32 pipe_width = Pipe()->GetWidth();
    i32 pipe_height = Pipe()->GetHeight();

    x = std::lround(pipe_width * params_.X);
    w = std::lround(pipe_width * (params_.X + params_.Width)) - x;

    i32 bottom = std::lround(pipe_height * params_.Y);
    h = std::lround(pipe_height * (params_.Y + params_.Height)) - bottom;

    y = pipe_height - (bottom + h);

    // Logical pixels so far - a fraction of agiPipeline::width_/height_, which is not the size of
    // the backbuffer this rectangle is about to be handed to. See the PRESENTATION TRANSFORM note in
    // dx9pipe.h; this is a no-op unless the two differ.
    //
    // Both callers want the mapped rectangle: Activate() sets it as the device viewport (which is
    // what puts hardware-transformed world geometry inside the same box the pretransformed content
    // lands in), and Clear() uses it as a scissor rect, which has to name the region actually being
    // drawn or it clears somewhere else.
    Pipe()->MapScreenRect(x, y, w, h);
}

void agiDX9Viewport::Activate()
{
    if (state_ == 0)
    {
        if (BeginGfx() != AGI_ERROR_SUCCESS)
            return;
    }

    ++agiViewParameters::ViewSerial;
    ++agiViewParameters::MtxSerial;

    agiViewport::Active = this;

    // Nothing here ever set the device viewport, so it stayed at its creation default - the whole
    // backbuffer - no matter which agiViewport was active. Sub-viewport rendering (the rear view
    // mirror, at 0.75/0.0 0.25x0.25) was therefore split in two: CPU-pretransformed content landed
    // correctly inside the mirror rectangle, because agiMeshSet::InitViewport() bakes the sub-rect
    // into HalfWidth/OffsX/OffsY, while everything on the hardware-transform path (MeshWorld) was
    // mapped to the *device* viewport and sprayed the mirror's view of the city across the entire
    // window, over the top of the main camera's frame. What was left visible inside the mirror box
    // was just its particles - tyre smoke, snow and rain - drawn in an otherwise empty rectangle.
    //
    // D3D9 applies this to both kinds of draw in the way each needs: transformed geometry gets the
    // viewport mapping, and pretransformed (XYZRHW) geometry keeps its absolute screen coordinates
    // but is clipped to the rectangle - which is where InitViewport() already placed it.
    i32 x, y, w, h;
    GetPixelRect(x, y, w, h);

    D3DVIEWPORT9 viewport {};
    viewport.X = static_cast<DWORD>(std::max(x, 0));
    viewport.Y = static_cast<DWORD>(std::max(y, 0));
    viewport.Width = static_cast<DWORD>(std::max(w, 0));
    viewport.Height = static_cast<DWORD>(std::max(h, 0));
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;

    Pipe()->Context()->GetDevice()->SetViewport(&viewport);
}

void agiDX9Viewport::SetBackground(aconst Vector3& color)
{
    clear_color_ = color;
}

void agiDX9Viewport::Clear(i32 flags)
{
    ARTS_UTIMED(agiClearViewport);

    // A clear writes D3DRS_ZWRITEENABLE straight to the device, and that is one of the states the
    // world path now mirrors (see agiDX9Rasterizer::LeaveWorldState). Hand the device back first, so
    // the mirror is dropped rather than left describing a value this is about to overwrite.
    Pipe()->Rast()->LeaveWorldState();

    IDirect3DDevice9* device = Pipe()->Context()->GetDevice();

    DWORD mask = 0;

    if (flags & AGI_VIEW_CLEAR_ZBUFFER)
    {
        mask |= D3DCLEAR_ZBUFFER;

        agiCurState.SetZWrite(true);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    }

    D3DCOLOR clear_color = 0;

    if (flags & AGI_VIEW_CLEAR_TARGET)
    {
        mask |= D3DCLEAR_TARGET;
        clear_color = D3DCOLOR_COLORVALUE(clear_color_.x, clear_color_.y, clear_color_.z, 1.0f);
    }

    if (mask)
    {
        // Was computing the scissor rect straight from params_.Y, which is measured from the
        // bottom (see GetPixelRect), and handing it to D3D9 as a top-left-origin RECT - so a
        // sub-viewport cleared the vertically mirrored region, not the one it draws into.
        i32 x, y, w, h;
        GetPixelRect(x, y, w, h);

        RECT rect {x, y, x + w, y + h};

        device->SetScissorRect(&rect);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);

        device->Clear(0, nullptr, mask, clear_color, 1.0f, 0);

        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    }
}

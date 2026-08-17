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

#include "agi/texdef.h"

#include "vector7/vector3.h"

#include "dx9pipe.h"

struct IDirect3DTexture9;

// Forget what stage 0's sampler state was last set to. Sampler state is global to the device and
// SetFilters() only issues a call when the value it wants differs from what it last wrote - so
// after a device Reset(), which returns every sampler to the D3D9 defaults (POINT/NONE/WRAP), the
// mirror describes a device that no longer exists and every write it would have made is skipped.
// Called from agiDX9OnDeviceReset() (dx9rsys.h), which is the one place that knows a reset happened.
void agiDX9InvalidateSamplerCache();

class agiDX9TexDef final : public agiTexDef
{
public:
    agiDX9TexDef(agiPipeline* pipe)
        : agiTexDef(pipe)
    {}

    i32 BeginGfx() override;
    void EndGfx() override;
    void Set(Vector2& arg1, Vector2& arg2) override;

    // Assumes texture is actively bound. u32 values are D3DTEXTUREFILTERTYPE, kept out of the
    // header to avoid pulling <d3d9.h> in everywhere agiDX9TexDef is forward-used.
    //
    // Also applies this texture's wrap/clamp addressing (agiTexParameters::WrapU/WrapV), which is
    // per-texture data but lives in *global* sampler state - see the cache note in the .cpp.
    void SetFilters(u32 min, u32 mag, u32 mip);

    b32 Lock(agiTexLock& lock) override;
    void Unlock(agiTexLock& lock) override;

    b32 IsAvailable() override;
    void Request() override;

    IDirect3DTexture9* GetHandle();

    // Average colour of the glow texture, on a coarse grid, for glow-driven lighting.
    //
    // A grid rather than one average because a single texture routinely holds several differently
    // coloured lamps - a traffic light sheet has red, amber and green side by side, and
    // agiMeshSet::DrawCard picks between them by UV sub-rect. One average over the whole sheet
    // would give every state the same muddy colour.
    //
    // Luminance-weighted: an AlphaGlow texture is mostly black background by area (that is what
    // makes it composite additively), so a flat mean would drag every light towards black. Weighting
    // by brightness makes the result the colour of the lamp rather than the colour of the padding.
    static constexpr u32 GlowGridSize = 4;

    Vector3 SampleGlowColor(f32 u, f32 v) const;

    bool HasGlowColors() const
    {
        return glow_colors_valid_;
    }

    agiDX9Pipeline* Pipe() const
    {
        return static_cast<agiDX9Pipeline*>(agiRefreshable::Pipe());
    }

private:
    IDirect3DTexture9* texture_ {};

    void BuildGlowColors(const agiSurfaceDesc& surface);

    Vector3 glow_colors_[GlowGridSize * GlowGridSize] {};
    bool glow_colors_valid_ {};

    Ptr<agiSurfaceDesc> temp_surface_;
    bool touched_ {};
};

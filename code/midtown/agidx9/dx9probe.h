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

class Vector3;

struct IDirect3DDevice9;
struct IDirect3DCubeTexture9;

// The environment cubemap cars reflect.
//
// WHY THIS IS SYNTHESISED RATHER THAN CAPTURED. The obvious way to build a reflection probe is to
// render the scene into six faces, and it is the wrong way here. Six extra scene renders a frame is
// not affordable on a backend that still has no frustum culling on the world path
// (dx9_rendering_pathways.md §1.3) and re-uploads every mesh through DrawIndexedPrimitiveUP, and
// the result would be dominated by sky anyway - a car sees sky above the horizon, road below it,
// and buildings only in a thin band. So the probe is computed analytically from the same sun,
// sky and ground the rest of the frame is lit by. It costs a fraction of a millisecond, only when
// the inputs actually change, and it agrees with the direct lighting by construction.
//
// It is a D3DPOOL_MANAGED texture rather than a render target, which means it survives a device
// reset with no work and needs nothing from agiDX9DefaultPoolResource.
//
// Roughness is handled by the mip chain. Rather than filtering across cube faces - which is fiddly
// and needs seam handling - each mip is generated independently from the same analytic sky with a
// progressively broader sun lobe and a softer horizon, which is what prefiltering an analytic
// environment converges to anyway. Mip 0 is a mirror, the last mip is near-uniform.
class agiDX9SkyProbe
{
public:
    bool Init(IDirect3DDevice9* device);
    void Shutdown();

    bool IsValid() const
    {
        return texture_ != nullptr;
    }

    // Rebuilds the faces, but only when one of the inputs has actually moved - so this is free on
    // the overwhelming majority of frames. Time of day and weather change a handful of times a
    // session; the sun does not move within a preset.
    void Update(
        const Vector3& sun_dir, const Vector3& sun_color, const Vector3& sky_color, const Vector3& ground_color);

    IDirect3DCubeTexture9* GetTexture() const
    {
        return texture_;
    }

    f32 GetMipCount() const
    {
        return static_cast<f32>(mip_count_);
    }

private:
    void Build();

    IDirect3DCubeTexture9* texture_ {};
    u32 mip_count_ {};
    bool half_float_ {};

    // Last inputs, for the change test.
    f32 last_[12] {};
    bool built_ {};
};

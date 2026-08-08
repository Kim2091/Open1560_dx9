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

#include "dx9probe.h"

#include "vector7/vector3.h"

#include "dx9_windows.h"

#include <cmath>

define_dummy_symbol(agidx9_dx9probe);

// 32x32 faces. The probe holds sky, a horizon band and a sun lobe - there is no detail in it finer
// than that, and a car's paint blurs whatever it reflects. Larger costs build time for nothing.
static constexpr u32 kFaceSize = 32;
static constexpr u32 kMipCount = 6; // 32, 16, 8, 4, 2, 1

// float -> half. Only needs to handle the normal range: everything written here is a non-negative
// colour, so denormals, infinities and NaN are out of scope by construction.
static u16 ToHalf(f32 value)
{
    if (!(value > 0.0f)) // also catches NaN
        return 0;

    if (value > 65504.0f)
        value = 65504.0f;

    const u32 bits = *reinterpret_cast<const u32*>(&value);
    const i32 exponent = static_cast<i32>((bits >> 23) & 0xFF) - 127 + 15;

    if (exponent <= 0)
        return 0; // underflows to zero rather than a denormal half

    if (exponent >= 31)
        return 0x7BFF; // largest finite half

    return static_cast<u16>((exponent << 10) | ((bits >> 13) & 0x3FF));
}

// Direction of the texel at (u, v) in [-1, 1] on `face`, in the D3D cube face order
// (+X, -X, +Y, -Y, +Z, -Z).
static Vector3 FaceDirection(u32 face, f32 u, f32 v)
{
    Vector3 d {};

    switch (face)
    {
        case 0: d = {1.0f, -v, -u}; break;
        case 1: d = {-1.0f, -v, u}; break;
        case 2: d = {u, 1.0f, v}; break;
        case 3: d = {u, -1.0f, -v}; break;
        case 4: d = {u, -v, 1.0f}; break;
        default: d = {-u, -v, -1.0f}; break;
    }

    const f32 length = std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));

    if (length > 0.0f)
    {
        d.x /= length;
        d.y /= length;
        d.z /= length;
    }

    return d;
}

bool agiDX9SkyProbe::Init(IDirect3DDevice9* device)
{
    Shutdown();

    if (!device)
        return false;

    IDirect3D9* d3d = nullptr;

    if (FAILED(device->GetDirect3D(&d3d)) || !d3d)
        return false;

    D3DDISPLAYMODE mode {};
    d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode);

    // A16B16G16R16F for the sun lobe, which is genuinely HDR - the whole point of a reflection
    // probe on car paint is that the sun reads as a hot highlight rather than a white blob clipped
    // at 1.0. Falls back to 8-bit rather than failing; a clamped probe still beats none.
    //
    // Filtering matters here, unlike the light and cell lookup textures: a 32x32 face magnified
    // across a car bonnet is visibly blocky under point sampling. So the format is checked for
    // D3DUSAGE_QUERY_FILTER, not merely for existence.
    half_float_ = SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format,
        D3DUSAGE_QUERY_FILTER, D3DRTYPE_CUBETEXTURE, D3DFMT_A16B16G16R16F));

    const D3DFORMAT format = half_float_ ? D3DFMT_A16B16G16R16F : D3DFMT_A8R8G8B8;

    d3d->Release();

    const HRESULT hr = device->CreateCubeTexture(kFaceSize, kMipCount, 0, format, D3DPOOL_MANAGED, &texture_, nullptr);

    if (FAILED(hr))
    {
        Warningf("DX9 probe: CreateCubeTexture failed, code %x", static_cast<u32>(hr));
        return false;
    }

    mip_count_ = kMipCount;
    built_ = false;

    Displayf("DX9 probe: %ux%u cube, %u mips, %s", kFaceSize, kFaceSize, kMipCount,
        half_float_ ? "A16B16G16R16F" : "A8R8G8B8 (no HDR)");

    return true;
}

void agiDX9SkyProbe::Shutdown()
{
    if (texture_)
    {
        texture_->Release();
        texture_ = nullptr;
    }

    mip_count_ = 0;
    built_ = false;
}

void agiDX9SkyProbe::Update(
    const Vector3& sun_dir, const Vector3& sun_color, const Vector3& sky_color, const Vector3& ground_color)
{
    if (!texture_)
        return;

    const f32 wanted[12] {sun_dir.x, sun_dir.y, sun_dir.z, sun_color.x, sun_color.y, sun_color.z, sky_color.x,
        sky_color.y, sky_color.z, ground_color.x, ground_color.y, ground_color.z};

    if (built_)
    {
        bool changed = false;

        for (u32 i = 0; i < 12; ++i)
        {
            if (std::fabs(wanted[i] - last_[i]) > 1e-4f)
            {
                changed = true;
                break;
            }
        }

        if (!changed)
            return;
    }

    std::memcpy(last_, wanted, sizeof(last_));
    built_ = true;

    Build();
}

void agiDX9SkyProbe::Build()
{
    const Vector3 sun_dir {last_[0], last_[1], last_[2]};
    const Vector3 sun_color {last_[3], last_[4], last_[5]};
    const Vector3 sky_color {last_[6], last_[7], last_[8]};
    const Vector3 ground_color {last_[9], last_[10], last_[11]};

    for (u32 mip = 0; mip < mip_count_; ++mip)
    {
        const u32 size = kFaceSize >> mip;

        // Roughness this mip stands for, 0 at the top and 1 at the bottom. Everything below is a
        // function of it: a rough surface sees a broad, soft sun and a washed-out horizon.
        const f32 roughness = (mip_count_ > 1) ? (static_cast<f32>(mip) / static_cast<f32>(mip_count_ - 1)) : 0.0f;

        // A mirror sees a small hard sun; a rough surface sees a large soft one. The exponent is
        // what a GGX lobe's width does with roughness, near enough for a 32x32 probe.
        const f32 sun_power = 2048.0f * std::pow(0.02f, roughness);
        const f32 sun_gain = 12.0f * std::pow(0.15f, roughness);

        // The horizon softens with roughness too, so the sky/ground boundary stops being a visible
        // line in a blurry reflection.
        const f32 horizon_sharpness = 8.0f * std::pow(0.2f, roughness);

        for (u32 face = 0; face < 6; ++face)
        {
            D3DLOCKED_RECT locked {};

            if (FAILED(texture_->LockRect(static_cast<D3DCUBEMAP_FACES>(face), mip, &locked, nullptr, 0)))
                continue;

            u8* row_base = static_cast<u8*>(locked.pBits);

            for (u32 y = 0; y < size; ++y)
            {
                u8* row = row_base + (static_cast<usize>(y) * locked.Pitch);

                for (u32 x = 0; x < size; ++x)
                {
                    const f32 u = ((static_cast<f32>(x) + 0.5f) / static_cast<f32>(size)) * 2.0f - 1.0f;
                    const f32 v = ((static_cast<f32>(y) + 0.5f) / static_cast<f32>(size)) * 2.0f - 1.0f;

                    const Vector3 dir = FaceDirection(face, u, v);

                    // Sky above, ground below, with a soft transition rather than a hard seam.
                    const f32 up = std::tanh(dir.y * horizon_sharpness) * 0.5f + 0.5f;

                    f32 r = ground_color.x + ((sky_color.x - ground_color.x) * up);
                    f32 g = ground_color.y + ((sky_color.y - ground_color.y) * up);
                    f32 b = ground_color.z + ((sky_color.z - ground_color.z) * up);

                    // Horizon haze: the sky is brightest where it meets the ground, which is what
                    // makes a reflection read as outdoors rather than as a two-tone gradient.
                    const f32 haze = std::pow(1.0f - std::fabs(dir.y), 8.0f) * 0.35f;

                    r += sky_color.x * haze;
                    g += sky_color.y * haze;
                    b += sky_color.z * haze;

                    // The sun itself.
                    const f32 cos_sun = (dir.x * sun_dir.x) + (dir.y * sun_dir.y) + (dir.z * sun_dir.z);

                    if (cos_sun > 0.0f)
                    {
                        const f32 lobe = std::pow(cos_sun, sun_power) * sun_gain;

                        r += sun_color.x * lobe;
                        g += sun_color.y * lobe;
                        b += sun_color.z * lobe;
                    }

                    if (half_float_)
                    {
                        u16* texel = reinterpret_cast<u16*>(row) + (static_cast<usize>(x) * 4);

                        texel[0] = ToHalf(r);
                        texel[1] = ToHalf(g);
                        texel[2] = ToHalf(b);
                        texel[3] = ToHalf(1.0f);
                    }
                    else
                    {
                        u32* texel = reinterpret_cast<u32*>(row) + x;

                        const auto to_byte = [](f32 value) -> u32 {
                            const f32 clamped = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
                            return static_cast<u32>(clamped * 255.0f + 0.5f);
                        };

                        *texel = 0xFF000000u | (to_byte(r) << 16) | (to_byte(g) << 8) | to_byte(b);
                    }
                }
            }

            texture_->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), mip);
        }
    }
}

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

#include "agi/viewport.h"

#include "dx9pipe.h"

#include "vector7/vector3.h"

class agiDX9Viewport final : public agiViewport
{
public:
    agiDX9Viewport(agiDX9Pipeline* pipe)
        : agiViewport(pipe)
    {}

    void EndGfx() override;
    i32 BeginGfx() override;
    void Activate() override;
    void SetBackground(aconst Vector3& color) override;
    void Clear(i32 arg1) override;

    agiDX9Pipeline* Pipe() const
    {
        return static_cast<agiDX9Pipeline*>(agiRefreshable::Pipe());
    }

private:
    // This viewport's rectangle in backbuffer pixels, converted from agiViewParameters' bottom-up
    // fractional coordinates to D3D9's top-down pixel coordinates. See the .cpp.
    void GetPixelRect(i32& x, i32& y, i32& w, i32& h) const;

    Vector3 clear_color_ {};
};

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

#include "carcamcs.h"

// A mouse-driven orbital chase camera.
//
// This class does not exist in the original game, so unlike every other camera here it declares no
// imported or exported members, no GetClass() override, and no check_size:
//
//  * There is no GetMetaClass<CarCamCS>() specialization for a MetaClass to parent itself to, so
//    META_DEFINE_CHILD would not link. Inheriting CarCamCS::GetClass() means the engine simply
//    sees a CarCamCS, and nothing ever reflects on, allocates, or serialises this class.
//  * Its size is therefore unconstrained, unlike the classes whose layouts the assembly indexes.
//
// It also never joins the node tree. mmViewCS::Update calls the current camera's Update() through
// the vtable and copies its camera_ matrix out directly, so writing camera_ is the whole contract.
class OrbitCamCS final : public CarCamCS
{
public:
    OrbitCamCS() = default;
    ~OrbitCamCS() override = default;

    void Init(mmCar* car);

    void MakeActive() override;

    void Update() override;

    f32 Yaw {};
    f32 Pitch {};
    f32 Distance {};
    f32 Height {};
    f32 YawScale {};
    f32 PitchScale {};
    i32 PrevMouseX {};
    i32 PrevMouseY {};

private:
    void SyncMouse();
};

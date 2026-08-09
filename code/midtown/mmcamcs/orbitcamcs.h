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
// Mouse input drives TargetYaw/TargetPitch; Yaw/Pitch chase them with a frame-rate independent
// exponential filter, so the view is smooth and behaves the same at 30 and 300 fps. After a short
// idle the target eases back behind the car, the way a driving game's chase camera does.
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

    // Where the camera is now, and where the input wants it to be.
    f32 Yaw {};
    f32 Pitch {};
    f32 TargetYaw {};
    f32 TargetPitch {};

    f32 Distance {};
    f32 Height {};

    // Radians per mouse count. Negative by default: raw mouse motion drives both axes the wrong
    // way round for an orbital view.
    f32 YawScale {};
    f32 PitchScale {};

    f32 SmoothRate {};
    f32 RecenterDelay {};
    f32 IdleTime {};

    i32 PrevMouseX {};
    i32 PrevMouseY {};

private:
    // Re-reads the mouse accumulator without applying it, so a stretch of suppressed input never
    // arrives later as one accumulated jump.
    void SyncMouse();

    // Eases the target back behind the car once the mouse has been idle for RecenterDelay.
    void Recenter(f32 delta);
};

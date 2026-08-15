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

#include "camshake.h"
#include "carcamcs.h"

// The dashboard view, with a head on it.
//
// The eye is rigidly attached to the car - it has to be, because the whole point of the shot is that
// the car is around you, and a stabilised eye would let a replaced interior slide about inside its
// own bodywork. So the camera takes the car's basis outright, and everything this class adds is
// expressed in that frame rather than in the world's.
//
// What it adds is two things.
//
// A HEAD BOB, from the same mmCamShake the orbital camera uses, so both cameras react to the same
// car in the same way - but applied quite differently. From outside, shake reads best as roll; from
// the driver's seat what you feel is your own head moving, so most of the effect here is positional,
// with the fore/aft channel led by acceleration rather than by the vibration, which is what turns it
// from a rattle into someone being pushed back into their seat.
//
// FREE LOOK, so the player can look around the cabin and out of the side windows. There is nothing
// to see there yet - the shipped interiors are a texture on the inside of the windscreen - but this
// is the camera an RTX Remix interior replacement would be viewed through, and a fixed forward stare
// is no way to look at one. It is ordinary fixed-function mouse look: the eye does not move, only
// the direction it faces, and it eases back to centre once the mouse goes quiet.
//
// Like OrbitCamCS this class does not exist in the original game, so it declares no imported or
// exported members, no GetClass() override and no check_size - see the note there, which applies
// here for the same reasons.
class DashCamCS final : public CarCamCS
{
public:
    DashCamCS() = default;
    ~DashCamCS() override = default;

    // eye is the seat position in the car's own space, and comes from the PovCamCS this stands in
    // for, so a change to that camera's offset carries over rather than being duplicated here.
    void Init(mmCar* car, mmViewCS* view, const Vector3& eye);

    void MakeActive() override;

    void Update() override;

    // Seat position, in the car's local frame.
    Vector3 Eye {};

    // --- Free look -----------------------------------------------------------------------------

    b32 LookEnabled {};

    // Where the view is pointing within the cabin, and where the input wants it. Both are relative
    // to straight ahead, so zero is the ordinary dashboard view.
    f32 LookYaw {};
    f32 LookPitch {};
    f32 TargetLookYaw {};
    f32 TargetLookPitch {};

    // Radians per mouse count, and how fast the view chases the input.
    f32 YawScale {};
    f32 PitchScale {};
    f32 SmoothRate {};

    // Seconds of mouse idle before the view eases back to straight ahead; 0 holds it where it was
    // left.
    f32 RecenterDelay {};
    f32 IdleTime {};

    i32 PrevMouseX {};
    i32 PrevMouseY {};

    // --- Head bob ------------------------------------------------------------------------------

    mmCamShake Shake {};

    // Multiplies the rotational and positional halves of the bob independently, so the shot can be
    // dialled between "the car is vibrating" and "you are being thrown around" without retuning the
    // trauma model underneath it.
    f32 BobSway {};
    f32 BobShift {};

private:
    void SyncMouse();

    void Recenter(f32 delta);
};

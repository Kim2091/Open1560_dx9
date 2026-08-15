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

#include "vector7/vector3.h"

class mmCar;
class Matrix34;

// The trauma-driven camera shake, shared by every camera that wants one.
//
// This does not touch a camera. It maintains the STATE - how hard the car is currently being driven,
// and where in the noise that puts us - and hands out samples; what to do with them is the camera's
// business, and the two that use it do quite different things. OrbitCamCS rolls and nudges a chase
// view; DashCamCS bobs a head. Sharing the model rather than the application is what keeps them
// feeling like the same car without pretending they are the same shot.
//
// Four sources feed one normalised trauma value: engine revs, road roughness, longitudinal
// acceleration and impacts. The amplitude is its square, so low trauma stays imperceptible rather
// than a constant buzz. A turn gate sits over all four and stands the shake down while the view is
// being turned - vibration on top of a view that is already moving reads as instability rather than
// as speed.
//
// The displacement is layered value noise rather than per-frame randomness, which is what makes it
// read as vibration instead of jitter, and it is a continuous function of time, so it looks the same
// at any frame rate rather than strobing when the rate drops.
class mmCamShake
{
public:
    // Reads the tunables from the command line. Safe to call again; nothing here is per-race state.
    void Init();

    // Clears the running state. Call when the camera is installed, so a crash from a previous stint
    // does not shake the new one and the first frame's derivatives are not taken against stale data.
    void Reset();

    // One simulation step. car may be null, in which case the shake simply decays.
    //
    // mouse_turn_rate is how fast the player is swinging the view by hand, in radians per second, so
    // that a camera driven by the mouse can feed its own contribution into the turn gate. Pass 0
    // from a camera the mouse does not turn.
    void Update(const mmCar* car, const Matrix34* car_matrix, f32 delta, f32 mouse_turn_rate);

    // Current amplitude, 0..1, gusts included. Zero means there is nothing to apply and the caller
    // can skip the sampling below entirely.
    f32 Strength() const
    {
        return strength_;
    }

    // Independent noise channels, each in [-1, 1] at the current phase. They are decorrelated by
    // hash salt, so no two ever line up into a single diagonal wobble, and a caller may use as few
    // or as many as its shot calls for. Scale by Strength() and by whatever magnitude suits.
    f32 PitchNoise() const;
    f32 YawNoise() const;
    f32 RollNoise() const;
    f32 RightNoise() const;
    f32 UpNoise() const;
    f32 ForwardNoise() const;

    // The individual sources, for a camera that wants to weight them differently from the combined
    // trauma - a head bob that leans with acceleration, say, rather than merely vibrating with it.
    f32 EngineTrauma {};
    f32 RoughTrauma {};
    f32 AccelTrauma {};
    f32 ImpactTrauma {};

    // Smoothed 0..1 measure of how hard the view is being turned, from whichever of the car and the
    // mouse is turning faster.
    f32 TurnFactor {};

    // --- Tunables ----------------------------------------------------------------------------

    // Overall displacement multiplier. Separate from the weights below because trauma is clamped to
    // one, so those cannot push the shake past full strength - only this can.
    f32 Amplitude {};

    // Per-source weights into the combined trauma. Any can be set to zero independently.
    f32 EngineScale {};
    f32 RoughScale {};
    f32 ImpactScale {};
    f32 AccelScale {};

    // Fraction of the redline the engine shake starts building from, how many of the top gears it is
    // allowed in, how far it dips mid-shift, the lowest gear it is allowed in at all, and how much
    // of the top of the rev ramp is taken back off again.
    f32 RpmStart {};
    f32 GearSpan {};
    f32 ShiftDip {};
    f32 MinGear {};
    f32 RevRolloff {};

    // How much of the shake a full-rate turn removes. 1 silences it while turning, 0 is off.
    f32 TurnScale {};

private:
    // Phase of the noise, advanced at a rate that rises with trauma, and the slower phase that
    // drives the amplitude wander. Both are held still while paused so the shake freezes rather than
    // idling in place.
    f32 time_ {};
    f32 gust_time_ {};
    f32 strength_ {};

    // Previous-frame state for the derivatives the sources are built from.
    Vector3 prev_velocity_ {};
    f32 prev_suspension_[4] {};
    f32 prev_heading_ {};
    b32 has_history_ {};

    f32 Combine() const;
};

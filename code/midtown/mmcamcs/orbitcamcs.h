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

// A mouse-driven orbital chase camera with a speed-reactive shake.
//
// Mouse input drives TargetYaw/TargetPitch; Yaw/Pitch chase them with a frame-rate independent
// exponential filter, so the view is smooth and behaves the same at 30 and 300 fps. After a short
// idle the target eases back behind the car, the way a driving game's chase camera does.
//
// On top of that sits a trauma-driven shake. Four sources - engine revs, road roughness,
// longitudinal acceleration and impacts - each contribute to a single normalised trauma value, and
// the shake amplitude is its square, so low trauma stays imperceptible rather than a constant buzz.
// A turn gate sits over the top of all four: while the view is being turned, by the car or by the
// mouse, the shake stands down, because vibration on top of a view that is already moving reads as
// instability rather than as speed.
// The displacement itself is layered sine noise at incommensurate frequencies rather than
// per-frame randomness, which is what makes it read as vibration instead of jitter.
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

    // The view is needed only so UpdateView() can push a changed FOV through to the asCamera;
    // nothing else sets BaseCamCS::View for a camera the engine did not create itself.
    void Init(mmCar* car, mmViewCS* view);

    void MakeActive() override;

    void Update() override;

    // --- Orbit ---------------------------------------------------------------------------------

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

    // How far the camera follows the direction of travel rather than the way the car points.
    // 0 locks it to the tail, 1 sits fully behind the velocity and shows the most of a slide.
    f32 DriftBias {};

    i32 PrevMouseX {};
    i32 PrevMouseY {};

    // --- Speed response ------------------------------------------------------------------------

    // Smoothed 0..1 position within the framing speed band, driving distance, height and FOV.
    f32 SpeedFactor {};

    // Framing band, in mph, to match how the cars are actually specified. It opens from a
    // standstill and keeps climbing to the fastest car in the game, so a Panoz GTR-1 at 206 is
    // visibly further back and wider than a 140 car flat out.
    f32 FrameStartSpeed {};
    f32 FrameMaxSpeed {};

    // Fraction of the redline the engine shake starts building from, how many of the top gears it
    // is allowed in at all, and how far it dips while a shift is in progress.
    f32 ShakeRpmStart {};
    f32 ShakeGearSpan {};
    f32 ShakeShiftDip {};

    // Lowest forward gear the engine shake is allowed in at all - 1 is first gear. A hard floor
    // underneath ShakeGearSpan's fade, so a car with a short gearbox cannot reach full shake in
    // what is effectively second.
    f32 ShakeMinGear {};

    // How much of the rev band's top end is taken back off again. 0 keeps the original straight
    // ramp to the redline; higher flattens the last of it, so the shake still builds with the revs
    // but stops climbing as hard once the needle is already round.
    f32 ShakeRevRolloff {};

    // How much of the shake a full-rate turn removes. 1 silences it completely while turning.
    f32 ShakeTurnScale {};

    // Added to Distance/Height across the framing band; the camera eases back and drops slightly.
    f32 SpeedDistance {};
    f32 SpeedHeight {};

    // Degrees added to the FOV across the framing band. BaseFov remembers the value to widen from.
    f32 SpeedFov {};
    f32 BaseFov {};

    // --- Shake ---------------------------------------------------------------------------------

    // Overall displacement multiplier. Separate from the weights below because trauma is clamped
    // to one, so those cannot push the shake past full strength - only this can.
    f32 ShakeAmplitude {};

    // Per-source weights into the combined trauma. Any can be set to zero independently.
    f32 ShakeScale {};
    f32 ShakeRoughScale {};
    f32 ShakeImpactScale {};
    f32 ShakeAccelScale {};

    // Phase of the noise, advanced at a rate that rises with trauma, and the current amplitude.
    // Both are held still while paused so the shake freezes rather than idling in place. GustTime
    // runs much slower and drives the amplitude wander; it needs its own accumulator so that its
    // phase wrap lands on the lattice too.
    f32 ShakeTime {};
    f32 GustTime {};
    f32 ShakeAmount {};

    f32 EngineTrauma {};
    f32 RoughTrauma {};
    f32 AccelTrauma {};
    f32 ImpactTrauma {};

    // Smoothed 0..1 measure of how hard the view is being turned, from whichever of the car and
    // the mouse is turning faster. Scales the whole shake down rather than any one source, because
    // what makes shake unbearable in a corner is that it fights a camera that is already moving.
    f32 TurnFactor {};

    // Radians per second the mouse is currently yawing the camera at. Sampled in Update(), where
    // the mouse delta is, and consumed by UpdateTrauma().
    f32 MouseTurnRate {};

    // Previous-frame state for the derivatives the trauma sources are built from.
    Vector3 PrevVelocity {};
    f32 PrevSuspension[4] {};
    f32 PrevHeading {};
    b32 HasHistory {};

private:
    // Re-reads the mouse accumulator without applying it, so a stretch of suppressed input never
    // arrives later as one accumulated jump.
    void SyncMouse();

    // Eases the target back behind the car once the mouse has been idle for RecenterDelay.
    void Recenter(f32 delta);

    // Advances the four trauma sources from this frame's car state.
    void UpdateTrauma(f32 delta);

    // Combines the trauma sources into the amplitude the shake is driven at.
    f32 GetShakeAmount() const;
};

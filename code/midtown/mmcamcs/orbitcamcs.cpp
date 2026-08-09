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

define_dummy_symbol(mmcamcs_orbitcamcs);

#include "orbitcamcs.h"

#include "arts7/sim.h"
#include "eventq7/event.h"
#include "mmcar/car.h"
#include "mmcar/trailer.h"

static mem::cmd_param PARAM_orbitsens {"orbitsens", "Orbital camera mouse sensitivity, in radians per mouse count"};
static mem::cmd_param PARAM_orbitdist {"orbitdist", "Orbital camera distance from the car"};
static mem::cmd_param PARAM_orbitheight {"orbitheight", "Orbital camera height above the car's origin"};
static mem::cmd_param PARAM_orbitinvertx {"orbitinvertx", "Invert the orbital camera's horizontal axis"};
static mem::cmd_param PARAM_orbitinverty {"orbitinverty", "Invert the orbital camera's vertical axis"};
static mem::cmd_param PARAM_orbitsmooth {"orbitsmooth", "Orbital camera smoothing rate; higher is snappier, 0 is off"};
static mem::cmd_param PARAM_orbitrecenter {
    "orbitrecenter", "Seconds of mouse idle before the orbital camera eases back behind the car; 0 is off"};

static mem::cmd_param PARAM_orbitshake {"orbitshake", "Orbital camera speed shake strength; 0 is off"};
static mem::cmd_param PARAM_orbitshakerough {"orbitshakerough", "Orbital camera road roughness shake strength"};
static mem::cmd_param PARAM_orbitshakeimpact {"orbitshakeimpact", "Orbital camera collision impact shake strength"};
static mem::cmd_param PARAM_orbitshakeaccel {"orbitshakeaccel", "Orbital camera acceleration shake strength"};
static mem::cmd_param PARAM_orbitshakemin {"orbitshakemin", "Speed the shake starts ramping in at, in m/s"};
static mem::cmd_param PARAM_orbitshakemax {"orbitshakemax", "Speed the shake reaches full strength at, in m/s"};
static mem::cmd_param PARAM_orbitpullback {"orbitpullback", "Metres the camera eases back by at full speed"};
static mem::cmd_param PARAM_orbitpulldown {"orbitpulldown", "Metres the camera drops by at full speed"};
static mem::cmd_param PARAM_orbitfov {
    "orbitfov", "Degrees of FOV widening at full speed; 0 is off. Changes the projection matrix"};

// Kept clear of +/- PI/2 so the view never degenerates looking straight down or up.
static constexpr f32 OrbitPitchMin = -0.35f;
static constexpr f32 OrbitPitchMax = 1.35f;

static constexpr f32 OrbitPitchStart = 0.25f;

// How fast the recentre eases in. Deliberately far slower than the input filter, so it reads as the
// camera drifting home rather than being yanked.
static constexpr f32 OrbitRecenterRate = 2.0f;

// Below this speed the car is parked or crawling, and pulling the camera round behind it fights the
// player rather than helping. Metres per second.
static constexpr f32 OrbitRecenterMinSpeed = 2.5f;

// Largest mouse movement honoured in a single frame. Bounds a flick that arrives as one enormous
// delta - the mouse leaving the window, or a stretch of frames where input was suppressed.
static constexpr i32 OrbitMaxMouseStep = 250;

// Peak rotational displacement at full amplitude, in radians. Roll is allowed the most: rolling the
// view reads as violence without ever pointing the camera away from the car, which is why it
// carries most of the effect here. All three are small - shake that is obvious on a screenshot is
// far too strong in motion.
static constexpr f32 OrbitShakePitch = 0.016f;
static constexpr f32 OrbitShakeYaw = 0.013f;
static constexpr f32 OrbitShakeRoll = 0.024f;

// Peak positional displacement, in metres, along the camera's own right and up axes.
static constexpr f32 OrbitShakeOffset = 0.05f;

// The noise advances faster as trauma rises, so the vibration tightens with speed instead of
// simply growing. Cycles per second, before the octave multipliers.
static constexpr f32 OrbitShakeBaseRate = 11.0f;
static constexpr f32 OrbitShakeRateRange = 9.0f;

// Envelope rates. Attack is quicker than release everywhere: effects should arrive promptly and
// leave gently, or the camera feels twitchy.
static constexpr f32 OrbitSpeedEnvelopeRate = 2.5f;
static constexpr f32 OrbitRoughAttack = 14.0f;
static constexpr f32 OrbitRoughRelease = 5.0f;
static constexpr f32 OrbitAccelAttack = 10.0f;
static constexpr f32 OrbitAccelRelease = 3.5f;

// Impacts have no attack at all - they land on the frame they happen and then decay.
static constexpr f32 OrbitImpactDecay = 3.2f;

// Acceleration magnitude, in m/s^2, beyond which a frame counts as a collision rather than
// driving. Hard braking peaks around 10; anything near 30 is a wall.
static constexpr f32 OrbitImpactThreshold = 30.0f;
static constexpr f32 OrbitImpactRange = 55.0f;

// Longitudinal acceleration, in m/s^2, that produces full acceleration shake.
static constexpr f32 OrbitAccelReference = 11.0f;

// Mean suspension travel rate, in m/s, that produces full roughness shake.
static constexpr f32 OrbitRoughReference = 0.85f;

// Reduces an angle to [-PI, PI] so yaw never winds up and differences take the short way round.
static f32 OrbitWrapAngle(f32 angle)
{
    return std::remainder(angle, 2.0f * ARTS_PI);
}

// Frame-rate independent exponential approach: the same fraction of the remaining distance is
// covered per second regardless of how the frames fall.
static f32 OrbitBlend(f32 rate, f32 delta)
{
    return 1.0f - std::exp(-rate * delta);
}

// Asymmetric envelope follower - rises at one rate, falls at another.
static f32 OrbitEnvelope(f32 current, f32 target, f32 delta, f32 attack, f32 release)
{
    return current + ((target - current) * OrbitBlend((target > current) ? attack : release, delta));
}

// Three sine octaves at incommensurate frequencies. Because the ratios are irrational the sum has
// no audible period, which is what stops the shake settling into a visible rhythm; and because it
// is continuous in time it stays smooth at any frame rate, unlike per-frame randomness. Weights
// sum to one, so the result is bounded to [-1, 1].
static f32 OrbitShakeNoise(f32 time, f32 phase)
{
    return (std::sin(time + phase) * 0.6f) + (std::sin((time * 2.269f) + (phase * 3.1f)) * 0.3f) +
        (std::sin((time * 4.673f) + (phase * 7.7f)) * 0.1f);
}

void OrbitCamCS::Init(mmCar* car, mmViewCS* view)
{
    Car = car;
    View = view;
    CarMatrix = &Car->Sim.LCS.World;
    SetName(Car->Sim.GetNodeName());

    Distance = PARAM_orbitdist.get_or(12.0f);
    Height = PARAM_orbitheight.get_or(1.5f);
    SmoothRate = PARAM_orbitsmooth.get_or(14.0f);
    RecenterDelay = PARAM_orbitrecenter.get_or(1.5f);

    f32 sensitivity = PARAM_orbitsens.get_or(0.0045f);

    YawScale = PARAM_orbitinvertx.get_or(false) ? sensitivity : -sensitivity;
    PitchScale = PARAM_orbitinverty.get_or(false) ? sensitivity : -sensitivity;

    ShakeScale = PARAM_orbitshake.get_or(1.0f);
    ShakeRoughScale = PARAM_orbitshakerough.get_or(0.55f);
    ShakeImpactScale = PARAM_orbitshakeimpact.get_or(1.0f);
    ShakeAccelScale = PARAM_orbitshakeaccel.get_or(0.35f);

    ShakeStartSpeed = PARAM_orbitshakemin.get_or(24.0f);
    ShakeMaxSpeed = PARAM_orbitshakemax.get_or(62.0f);

    SpeedDistance = PARAM_orbitpullback.get_or(2.5f);
    SpeedHeight = PARAM_orbitpulldown.get_or(-0.35f);
    SpeedFov = PARAM_orbitfov.get_or(0.0f);

    // BaseCamCS defaults this to 3.0, which clips the car when orbiting in close.
    CameraNear = 0.5f;

    BaseFov = CameraFOV;
}

void OrbitCamCS::MakeActive()
{
    if (!Car)
        return;

    // The interior cameras deactivate the car model, so make sure it is drawn again.
    Car->Model.Activate();

    if (Car->Trailer)
        Car->Trailer->Inst.SetFlags(INST_FLAG_ACTIVE);

    // PolarView offsets along +Z before rotating, and the camera looks back towards the point it
    // orbits, so the car's heading on its own would place us in front of it looking at the nose.
    if (CarMatrix)
        TargetYaw = OrbitWrapAngle(std::atan2(CarMatrix->m2.x, CarMatrix->m2.z) + ARTS_PI);

    TargetPitch = OrbitPitchStart;

    // Snap rather than smooth into place: this runs as the camera is installed, and the transition
    // mmViewCS runs on top is already doing the blending from the previous camera.
    Yaw = TargetYaw;
    Pitch = TargetPitch;
    IdleTime = 0.0f;

    // Start from a clean slate so a crash from the previous stint does not shake the new one, and
    // so the first frame's derivatives are not taken against stale state.
    SpeedTrauma = 0.0f;
    RoughTrauma = 0.0f;
    AccelTrauma = 0.0f;
    ImpactTrauma = 0.0f;
    ShakeAmount = 0.0f;
    SpeedFactor = 0.0f;
    HasHistory = false;

    SyncMouse();
}

void OrbitCamCS::Update()
{
    f32 delta = Sim()->GetUpdateDelta();

    // Always re-read the accumulator, even when the input is being thrown away, so that resuming
    // never applies everything that happened while the camera was not listening.
    i32 mouse_x = PrevMouseX;
    i32 mouse_y = PrevMouseY;

    if (eqEventHandler* queue = eqEventHandler::SuperQ)
    {
        mouse_x = queue->GetMouseVirtualX();
        mouse_y = queue->GetMouseVirtualY();
    }

    i32 step_x = std::clamp(mouse_x - PrevMouseX, -OrbitMaxMouseStep, OrbitMaxMouseStep);
    i32 step_y = std::clamp(mouse_y - PrevMouseY, -OrbitMaxMouseStep, OrbitMaxMouseStep);

    PrevMouseX = mouse_x;
    PrevMouseY = mouse_y;

    // While the pause menu is up the mouse belongs to the menu, so the camera holds still. Nothing
    // below runs, which also freezes the shake rather than leaving it idling on a paused screen.
    if (!Sim()->IsPaused() && (delta > 0.0f))
    {
        if ((step_x != 0) || (step_y != 0))
        {
            TargetYaw = OrbitWrapAngle(TargetYaw + (static_cast<f32>(step_x) * YawScale));
            TargetPitch =
                std::clamp(TargetPitch + (static_cast<f32>(step_y) * PitchScale), OrbitPitchMin, OrbitPitchMax);

            IdleTime = 0.0f;
        }
        else
        {
            IdleTime += delta;

            Recenter(delta);
        }

        if (SmoothRate > 0.0f)
        {
            f32 blend = OrbitBlend(SmoothRate, delta);

            Yaw = OrbitWrapAngle(Yaw + (OrbitWrapAngle(TargetYaw - Yaw) * blend));
            Pitch += (TargetPitch - Pitch) * blend;
        }
        else
        {
            Yaw = TargetYaw;
            Pitch = TargetPitch;
        }

        UpdateTrauma(delta);

        ShakeAmount = GetShakeAmount();

        // Wrapped to keep single-precision resolution usable across a long session.
        ShakeTime =
            std::fmod(ShakeTime + (delta * (OrbitShakeBaseRate + (OrbitShakeRateRange * ShakeAmount))), 100000.0f);
    }

    f32 pitch = Pitch;
    f32 yaw = Yaw;
    f32 roll = 0.0f;

    if (ShakeAmount > 0.0f)
    {
        pitch += OrbitShakeNoise(ShakeTime, 0.0f) * ShakeAmount * OrbitShakePitch;
        yaw += OrbitShakeNoise(ShakeTime, 11.3f) * ShakeAmount * OrbitShakeYaw;
        roll = OrbitShakeNoise(ShakeTime, 23.7f) * ShakeAmount * OrbitShakeRoll;
    }

    camera_.PolarView(
        Distance + (SpeedDistance * SpeedFactor), yaw, std::clamp(pitch, OrbitPitchMin, OrbitPitchMax), roll);

    if (CarMatrix)
        camera_.m3 += CarMatrix->m3;

    camera_.m3.y += Height + (SpeedHeight * SpeedFactor);

    if (ShakeAmount > 0.0f)
    {
        // Along the camera's own right and up axes rather than world axes, so the jitter stays
        // perpendicular to the view and never pushes the camera into or away from the car.
        camera_.m3 += camera_.m0 * (OrbitShakeNoise(ShakeTime, 41.2f) * ShakeAmount * OrbitShakeOffset);
        camera_.m3 += camera_.m1 * (OrbitShakeNoise(ShakeTime, 57.6f) * ShakeAmount * OrbitShakeOffset);
    }

    // Off by default. CameraFOV only reaches the asCamera through UpdateView(), which nothing in
    // the original game calls, so this has to push it through itself.
    if (SpeedFov != 0.0f)
    {
        f32 wanted = BaseFov + (SpeedFov * SpeedFactor);

        if (std::fabs(wanted - CameraFOV) > 0.05f)
        {
            CameraFOV = wanted;

            UpdateView();
        }
    }
}

void OrbitCamCS::UpdateTrauma(f32 delta)
{
    if (!Car)
        return;

    const mmCarSim& sim = Car->Sim;

    f32 span = ShakeMaxSpeed - ShakeStartSpeed;
    f32 speed_target = (span > 0.0f) ? std::clamp((sim.Speed - ShakeStartSpeed) / span, 0.0f, 1.0f) : 0.0f;

    SpeedFactor = OrbitEnvelope(SpeedFactor, speed_target, delta, OrbitSpeedEnvelopeRate, OrbitSpeedEnvelopeRate);
    SpeedTrauma = SpeedFactor;

    // Road roughness, read as how fast the suspension is moving. Only wheels actually on the
    // ground contribute - a wheel dangling in mid-air swings freely and would otherwise register
    // as the roughest road in the city.
    const mmWheel* wheels[4] {&sim.FrontLeft, &sim.FrontRight, &sim.BackLeft, &sim.BackRight};

    f32 travel = 0.0f;
    i32 grounded = 0;

    for (i32 i = 0; i < 4; ++i)
    {
        if (wheels[i]->OnGround)
        {
            travel += std::fabs(wheels[i]->Suspension - PrevSuspension[i]);
            ++grounded;
        }

        PrevSuspension[i] = wheels[i]->Suspension;
    }

    f32 rough_target = 0.0f;

    if (HasHistory && (grounded > 0))
    {
        rough_target = std::clamp((travel / (static_cast<f32>(grounded) * delta)) / OrbitRoughReference, 0.0f, 1.0f);
    }

    RoughTrauma = OrbitEnvelope(RoughTrauma, rough_target, delta, OrbitRoughAttack, OrbitRoughRelease);

    // One velocity derivative feeds both remaining sources: its component along the car's forward
    // axis is acceleration and braking, while its total magnitude is what a collision looks like.
    Vector3 velocity = sim.ICS.LinearVelocity;
    f32 accel_target = 0.0f;

    if (HasHistory)
    {
        Vector3 change = velocity - PrevVelocity;
        f32 magnitude = change.Mag() / delta;

        if (CarMatrix)
            accel_target = std::clamp(std::fabs(change ^ CarMatrix->m2) / delta / OrbitAccelReference, 0.0f, 1.0f);

        if (magnitude > OrbitImpactThreshold)
        {
            ImpactTrauma =
                std::max(ImpactTrauma, std::clamp((magnitude - OrbitImpactThreshold) / OrbitImpactRange, 0.0f, 1.0f));
        }
    }

    PrevVelocity = velocity;

    AccelTrauma = OrbitEnvelope(AccelTrauma, accel_target, delta, OrbitAccelAttack, OrbitAccelRelease);
    ImpactTrauma *= std::exp(-OrbitImpactDecay * delta);

    HasHistory = true;
}

f32 OrbitCamCS::GetShakeAmount() const
{
    f32 trauma = std::clamp((SpeedTrauma * ShakeScale) + (RoughTrauma * ShakeRoughScale) +
            (AccelTrauma * ShakeAccelScale) + (ImpactTrauma * ShakeImpactScale),
        0.0f, 1.0f);

    // Squaring keeps low trauma imperceptible rather than a permanent low buzz, and makes the ramp
    // towards top speed feel like it builds instead of arriving linearly.
    return trauma * trauma;
}

void OrbitCamCS::Recenter(f32 delta)
{
    if ((RecenterDelay <= 0.0f) || (IdleTime < RecenterDelay) || !Car || !CarMatrix)
        return;

    // Leave a parked car's camera where the player put it.
    if (Car->Sim.Speed < OrbitRecenterMinSpeed)
        return;

    f32 desired = std::atan2(CarMatrix->m2.x, CarMatrix->m2.z) + ARTS_PI;
    f32 blend = OrbitBlend(OrbitRecenterRate, delta);

    TargetYaw = OrbitWrapAngle(TargetYaw + (OrbitWrapAngle(desired - TargetYaw) * blend));
    TargetPitch += (OrbitPitchStart - TargetPitch) * blend;
}

void OrbitCamCS::SyncMouse()
{
    if (eqEventHandler* queue = eqEventHandler::SuperQ)
    {
        PrevMouseX = queue->GetMouseVirtualX();
        PrevMouseY = queue->GetMouseVirtualY();
    }
}

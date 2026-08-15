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

define_dummy_symbol(mmcamcs_dashcamcs);

#include "dashcamcs.h"

#include "arts7/sim.h"
#include "eventq7/event.h"
#include "mmcar/car.h"

static mem::cmd_param PARAM_dashlook {"dashlook", "Mouse look from the dashboard camera; 0 locks it forward"};
static mem::cmd_param PARAM_dashlooksens {"dashlooksens", "Dashboard look sensitivity, in radians per mouse count"};
static mem::cmd_param PARAM_dashlooksmooth {"dashlooksmooth", "Dashboard look smoothing rate; 0 is off"};
static mem::cmd_param PARAM_dashlookrecenter {
    "dashlookrecenter", "Seconds of mouse idle before the dashboard view eases back to centre; 0 is off"};
static mem::cmd_param PARAM_dashlookinvertx {"dashlookinvertx", "Invert the dashboard look's horizontal axis"};
static mem::cmd_param PARAM_dashlookinverty {"dashlookinverty", "Invert the dashboard look's vertical axis"};
static mem::cmd_param PARAM_dashbob {"dashbob", "Dashboard head bob strength; 0 is off"};
static mem::cmd_param PARAM_dashbobsway {"dashbobsway", "How much the dashboard head bob rotates the view"};

// How far the head may turn, in radians. Yaw reaches a little past the side window without letting
// the player look through their own seat; pitch covers the instruments to the top of the windscreen.
static constexpr f32 DashLookYawLimit = 2.0f;
static constexpr f32 DashLookPitchMin = -0.9f;
static constexpr f32 DashLookPitchMax = 0.7f;

// How fast the view eases back to straight ahead once the mouse goes quiet. Slower than the input
// filter, so it reads as the head settling rather than snapping forward.
static constexpr f32 DashRecenterRate = 3.0f;

// Largest mouse movement honoured in a single frame - see OrbitCamCS, same reasoning.
static constexpr i32 DashMaxMouseStep = 250;

// Peak rotational displacement of the bob at full amplitude, in radians.
//
// Smaller than the orbital camera's, and weighted differently: from the driver's seat the view is
// the head, so rotation reads as being shaken by the neck and goes wrong very quickly, while the
// same motion applied to the eye POSITION reads as the whole body moving with the car. Roll is
// allowed the most of the three because it is the one a real head genuinely does.
static constexpr f32 DashBobPitch = 0.010f;
static constexpr f32 DashBobYaw = 0.008f;
static constexpr f32 DashBobRoll = 0.018f;

// Peak positional displacement, in metres, along the car's own axes. Larger than the orbital
// camera's, which is the whole difference between a camera vibrating and a head bobbing.
static constexpr f32 DashBobRight = 0.030f;
static constexpr f32 DashBobUp = 0.045f;
static constexpr f32 DashBobForward = 0.025f;

// How far the head is thrown fore and aft by acceleration alone, in metres, on top of the noise.
//
// This is the part that makes it read as a person rather than a camera mount. The noise channels
// vibrate about a fixed point; this leans, and it leans with the same acceleration the trauma model
// is already measuring, so hard acceleration pushes the head back into the seat and braking pitches
// it towards the wheel. Deliberately not fed through the shake's own strength - it is a lean, not a
// vibration, and it should be there at any speed the car is actually accelerating at.
static constexpr f32 DashLeanAccel = 0.055f;

// Which way round this engine's camera matrix runs.
//
// Every other camera here builds its matrix through Matrix34::PolarView and never has to know
// whether m2 is the direction the camera looks or the direction it looks away along - PolarView
// settles it for them. This one cannot use PolarView, because its angles are the world's and the
// whole point of a dashboard camera is that its angles are the CAR's, so it writes the basis itself
// and does need to know.
//
// Rather than assume - the geometry of offset-then-rotate is famously easy to reason the wrong way
// round here, and getting it wrong yields a camera that faces out of the back window - this asks the
// engine. PolarView places the eye at a distance from what it is looking at, so for a one-metre
// offset the direction from the eye back to the target IS the direction the camera looks, whatever
// the convention. Comparing that against the matrix's own axes gives the signs, and any convention
// this function itself imposes (the order of the cross product below) is absorbed by the same
// comparison.
struct DashBasisSigns
{
    f32 Right {1.0f};
    f32 Up {1.0f};
    f32 Forward {1.0f};
};

static DashBasisSigns DashProbeBasisSigns()
{
    static constexpr Vector3 WorldUp {0.0f, 1.0f, 0.0f};

    Matrix34 reference;
    reference.PolarView(1.0f, 0.0f, 0.0f, 0.0f);

    DashBasisSigns signs;

    const f32 mag2 = reference.m3.Mag2();

    // The offset did not translate, so there is nothing to calibrate against. Take the axes at face
    // value; a forward-facing guess is the better failure.
    if (mag2 < 1.0e-6f)
        return signs;

    const Vector3 look = reference.m3 * (-1.0f / std::sqrt(mag2));

    Vector3 look_right;
    look_right.Cross(WorldUp, look);

    signs.Forward = ((reference.m2 ^ look) >= 0.0f) ? 1.0f : -1.0f;
    signs.Up = ((reference.m1 ^ WorldUp) >= 0.0f) ? 1.0f : -1.0f;
    signs.Right = ((reference.m0 ^ look_right) >= 0.0f) ? 1.0f : -1.0f;

    return signs;
}

// Reduces an angle to [-PI, PI].
static f32 DashWrapAngle(f32 angle)
{
    return std::remainder(angle, 2.0f * ARTS_PI);
}

static f32 DashBlend(f32 rate, f32 delta)
{
    return 1.0f - std::exp(-rate * delta);
}

void DashCamCS::Init(mmCar* car, mmViewCS* view, const Vector3& eye)
{
    Car = car;
    View = view;
    Eye = eye;

    if (Car)
        SetName(Car->Sim.GetNodeName());

    LookEnabled = PARAM_dashlook.get_or(true);

    f32 sensitivity = PARAM_dashlooksens.get_or(0.0035f);

    // Negative by default for the same reason the orbital camera's are: raw mouse motion drives both
    // axes the wrong way round for a view that turns with the mouse rather than being dragged by it.
    YawScale = PARAM_dashlookinvertx.get_or(false) ? sensitivity : -sensitivity;
    PitchScale = PARAM_dashlookinverty.get_or(false) ? sensitivity : -sensitivity;

    SmoothRate = PARAM_dashlooksmooth.get_or(16.0f);
    RecenterDelay = PARAM_dashlookrecenter.get_or(1.2f);

    BobShift = PARAM_dashbob.get_or(1.0f);
    BobSway = PARAM_dashbobsway.get_or(1.0f);

    Shake.Init();

    // Close in: the eye is inside the cabin and the windscreen pillars are centimetres away, so the
    // default near plane clips straight through them.
    CameraNear = 0.1f;
}

void DashCamCS::MakeActive()
{
    LookYaw = 0.0f;
    LookPitch = 0.0f;
    TargetLookYaw = 0.0f;
    TargetLookPitch = 0.0f;
    IdleTime = 0.0f;

    Shake.Reset();

    SyncMouse();
}

void DashCamCS::Update()
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

    i32 step_x = std::clamp(mouse_x - PrevMouseX, -DashMaxMouseStep, DashMaxMouseStep);
    i32 step_y = std::clamp(mouse_y - PrevMouseY, -DashMaxMouseStep, DashMaxMouseStep);

    PrevMouseX = mouse_x;
    PrevMouseY = mouse_y;

    if (!Sim()->IsPaused() && (delta > 0.0f))
    {
        f32 mouse_turn_rate = 0.0f;

        if (LookEnabled)
        {
            mouse_turn_rate = std::fabs(static_cast<f32>(step_x) * YawScale) / delta;

            if ((step_x != 0) || (step_y != 0))
            {
                TargetLookYaw = std::clamp(
                    TargetLookYaw + (static_cast<f32>(step_x) * YawScale), -DashLookYawLimit, DashLookYawLimit);
                TargetLookPitch = std::clamp(
                    TargetLookPitch + (static_cast<f32>(step_y) * PitchScale), DashLookPitchMin, DashLookPitchMax);

                IdleTime = 0.0f;
            }
            else
            {
                IdleTime += delta;

                Recenter(delta);
            }

            if (SmoothRate > 0.0f)
            {
                f32 blend = DashBlend(SmoothRate, delta);

                LookYaw += DashWrapAngle(TargetLookYaw - LookYaw) * blend;
                LookPitch += (TargetLookPitch - LookPitch) * blend;
            }
            else
            {
                LookYaw = TargetLookYaw;
                LookPitch = TargetLookPitch;
            }
        }

        Shake.Update(Car, Car ? &Car->Sim.LCS.World : nullptr, delta, mouse_turn_rate);
    }

    const Matrix34* car_matrix = Car ? &Car->Sim.LCS.World : nullptr;

    if (!car_matrix)
        return;

    const f32 shake = Shake.Strength();

    // Rotation of the head within the cabin: where the player is looking, plus the sway.
    //
    // Built here rather than through Matrix34::PolarView because this one is expressed in the CAR's
    // frame, not the world's, and PolarView's angles are the world's. Yaw about the car's up axis
    // then pitch about the yawed right axis, which is the order that keeps the horizon level as the
    // head turns; roll last, about the resulting view direction.
    const f32 yaw = LookYaw + (shake > 0.0f ? (Shake.YawNoise() * shake * DashBobYaw * BobSway) : 0.0f);
    const f32 pitch = LookPitch + (shake > 0.0f ? (Shake.PitchNoise() * shake * DashBobPitch * BobSway) : 0.0f);
    const f32 roll = (shake > 0.0f) ? (Shake.RollNoise() * shake * DashBobRoll * BobSway) : 0.0f;

    const f32 cy = std::cos(yaw);
    const f32 sy = std::sin(yaw);
    const f32 cp = std::cos(pitch);
    const f32 sp = std::sin(pitch);
    const f32 cr = std::cos(roll);
    const f32 sr = std::sin(roll);

    // Head axes in the car's frame: right, up, forward. At zero this is the identity, i.e. the eye
    // simply looks the way the car does, which is the property the whole shot rests on.
    const Vector3 forward {sy * cp, sp, cy * cp};
    const Vector3 right_flat {cy, 0.0f, -sy};
    const Vector3 up_pitched {-sy * sp, cp, -cy * sp};

    const Vector3 right = (right_flat * cr) - (up_pitched * sr);
    const Vector3 up = (right_flat * sr) + (up_pitched * cr);

    // Into world space. Composed against the car's own basis rather than a world-space heading, so
    // the view inherits the car's roll and pitch exactly - over a jump, up a kerb, through a banked
    // corner - and an interior replaced by RTX Remix stays welded to the bodywork around it.
    const auto to_world = [&](const Vector3& local) {
        return (car_matrix->m0 * local.x) + (car_matrix->m1 * local.y) + (car_matrix->m2 * local.z);
    };

    const DashBasisSigns signs = DashProbeBasisSigns();

    camera_.m0 = to_world(right) * signs.Right;
    camera_.m1 = to_world(up) * signs.Up;
    camera_.m2 = to_world(forward) * signs.Forward;

    // The seat, in world space.
    Vector3 seat = Eye;

    if (shake > 0.0f)
    {
        // The bob proper. Along the CAR's axes rather than the head's, because what moves is the
        // body in the seat; applying it along the view would swing the eye sideways as the player
        // looked around, which is the one thing a head bob must never do.
        seat.x += Shake.RightNoise() * shake * DashBobRight * BobShift;
        seat.y += Shake.UpNoise() * shake * DashBobUp * BobShift;
        seat.z += Shake.ForwardNoise() * shake * DashBobForward * BobShift;
    }

    // The lean, which is not part of the shake and does not go through its strength - see
    // DashLeanAccel. AccelTrauma is unsigned (it measures how hard, not which way), so the direction
    // comes from the sign of the car's own forward velocity change, recovered here from whether it
    // is gaining or losing speed along its own axis.
    if (Car)
    {
        const f32 forward_speed = Car->Sim.ICS.LinearVelocity ^ car_matrix->m2;
        const f32 lean = Shake.AccelTrauma * DashLeanAccel * BobShift;

        seat.z -= (forward_speed >= 0.0f) ? lean : -lean;
    }

    camera_.m3 = car_matrix->m3 + to_world(seat);
}

void DashCamCS::Recenter(f32 delta)
{
    if ((RecenterDelay <= 0.0f) || (IdleTime < RecenterDelay))
        return;

    const f32 blend = DashBlend(DashRecenterRate, delta);

    TargetLookYaw -= TargetLookYaw * blend;
    TargetLookPitch -= TargetLookPitch * blend;
}

void DashCamCS::SyncMouse()
{
    if (eqEventHandler* queue = eqEventHandler::SuperQ)
    {
        PrevMouseX = queue->GetMouseVirtualX();
        PrevMouseY = queue->GetMouseVirtualY();
    }
}

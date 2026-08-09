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

void OrbitCamCS::Init(mmCar* car)
{
    Car = car;
    CarMatrix = &Car->Sim.LCS.World;
    SetName(Car->Sim.GetNodeName());

    Distance = PARAM_orbitdist.get_or(12.0f);
    Height = PARAM_orbitheight.get_or(1.5f);
    SmoothRate = PARAM_orbitsmooth.get_or(14.0f);
    RecenterDelay = PARAM_orbitrecenter.get_or(1.5f);

    f32 sensitivity = PARAM_orbitsens.get_or(0.0045f);

    YawScale = PARAM_orbitinvertx.get_or(false) ? sensitivity : -sensitivity;
    PitchScale = PARAM_orbitinverty.get_or(false) ? sensitivity : -sensitivity;

    // BaseCamCS defaults this to 3.0, which clips the car when orbiting in close.
    CameraNear = 0.5f;
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

    // While the pause menu is up the mouse belongs to the menu, so the camera holds still. It still
    // follows the car, which costs nothing and keeps the view sane if anything does move.
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
    }

    camera_.PolarView(Distance, Yaw, Pitch, 0.0f);

    if (CarMatrix)
        camera_.m3 += CarMatrix->m3;

    camera_.m3.y += Height;
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

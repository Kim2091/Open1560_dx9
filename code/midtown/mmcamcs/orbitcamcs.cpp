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

#include "eventq7/event.h"
#include "mmcar/car.h"
#include "mmcar/trailer.h"

static mem::cmd_param PARAM_orbitsens {"orbitsens", "Orbital camera mouse sensitivity, in radians per mouse count"};
static mem::cmd_param PARAM_orbitdist {"orbitdist", "Orbital camera distance from the car"};
static mem::cmd_param PARAM_orbitheight {"orbitheight", "Orbital camera height above the car's origin"};
static mem::cmd_param PARAM_orbitinvertx {"orbitinvertx", "Invert the orbital camera's horizontal axis"};
static mem::cmd_param PARAM_orbitinverty {"orbitinverty", "Invert the orbital camera's vertical axis"};

// Kept clear of +/- PI/2 so the view never degenerates looking straight down or up.
static constexpr f32 OrbitPitchMin = -0.35f;
static constexpr f32 OrbitPitchMax = 1.35f;

static constexpr f32 OrbitPitchStart = 0.25f;

void OrbitCamCS::Init(mmCar* car)
{
    Car = car;
    CarMatrix = &Car->Sim.LCS.World;
    SetName(Car->Sim.GetNodeName());

    Distance = PARAM_orbitdist.get_or(12.0f);
    Height = PARAM_orbitheight.get_or(1.5f);

    f32 sensitivity = PARAM_orbitsens.get_or(0.0045f);

    YawScale = PARAM_orbitinvertx.get_or(false) ? -sensitivity : sensitivity;
    PitchScale = PARAM_orbitinverty.get_or(false) ? -sensitivity : sensitivity;

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
        Yaw = std::atan2(CarMatrix->m2.x, CarMatrix->m2.z) + ARTS_PI;

    Pitch = OrbitPitchStart;

    SyncMouse();
}

void OrbitCamCS::Update()
{
    if (eqEventHandler* queue = eqEventHandler::SuperQ)
    {
        i32 mouse_x = queue->GetMouseVirtualX();
        i32 mouse_y = queue->GetMouseVirtualY();

        // Deltas are taken against the running accumulator rather than consumed, so being updated
        // more than once in a frame - as happens while a transition is running - adds nothing the
        // second time, and time spent on another camera never arrives as one accumulated jump.
        Yaw += static_cast<f32>(mouse_x - PrevMouseX) * YawScale;
        Pitch = std::clamp(Pitch + (static_cast<f32>(mouse_y - PrevMouseY) * PitchScale), OrbitPitchMin, OrbitPitchMax);

        PrevMouseX = mouse_x;
        PrevMouseY = mouse_y;
    }

    camera_.PolarView(Distance, Yaw, Pitch, 0.0f);

    if (CarMatrix)
        camera_.m3 += CarMatrix->m3;

    camera_.m3.y += Height;
}

void OrbitCamCS::SyncMouse()
{
    if (eqEventHandler* queue = eqEventHandler::SuperQ)
    {
        PrevMouseX = queue->GetMouseVirtualX();
        PrevMouseY = queue->GetMouseVirtualY();
    }
}

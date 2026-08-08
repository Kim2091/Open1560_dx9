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

define_dummy_symbol(mmeffects_vehform);

#include "vehform.h"

#include "agi/texdef.h"
#include "agiworld/quality.h"
#include "agiworld/texsheet.h"
#include "agiworld/texsort.h"
#include "arts7/cullmgr.h"
#include "mmcity/cullcity.h"
#include "mmcityinfo/state.h"

#include <algorithm>

static mem::cmd_param PARAM_menu_refl {"menurefl"};

mmVehicleForm::mmVehicleForm()
    : color_pointer(&color_index_)
{
    if (SphMapTex)
    {
        SphMapTex->AddRef();
    }
    else
    {
        if (agiRQ.SphMap && PARAM_menu_refl.get_or<bool>(true))
        {
            // The player's own preset, not a hardcoded one.
            //
            // This was &mmEnvSetup[1][0] - Noon/Clear - so the vehicle-select preview always
            // reflected a clear midday sky no matter what the race was set to, and a car previewed
            // before a night race looked nothing like the car that then drove out of it. The table
            // is indexed [TimeOfDay][Weather] exactly as mmCullCity::Init indexes it for the city.
            //
            // Bound once per showroom entry rather than continuously: SphMapTex is a shared static
            // whose lifetime is refcounted across every mmVehicleForm, so swapping it underneath
            // live holders would unbalance their Release(). The last form to die nulls it, and the
            // next entry re-reads the preset - which is the behaviour that matters here.
            const i32 time_index = std::clamp(static_cast<i32>(MMSTATE.TimeOfDay), 0, 3);
            const i32 weather_index = std::clamp(static_cast<i32>(MMSTATE.Weather), 0, 3);

            t_mmEnvSetup* env = &mmEnvSetup[time_index][weather_index];

            SphMapTex = as_raw GetPackedTexture(xconst(env->SphereMap), 0);

            if (SphMapTex)
                SphMapTex->Tex.Props |= agiTexProp::AlphaGlow;
        }
    }
}

mmVehicleForm::~mmVehicleForm()
{
    if (SphMapTex)
    {
        if (SphMapTex->Release() == 0)
            SphMapTex = nullptr;
    }
}

void mmVehicleForm::Update()
{
    if (vehicle_mesh_ && shadow_mesh_)
    {
        CullMgr()->DeclareCullable(this);
    }
}

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

define_dummy_symbol(mmcity_cellrend);

#include "cellrend.h"

#include "agi/rsys.h"
#include "agiworld/quality.h"
#include "mmcity/inst.h"

f32 ObjectMaxDist = 300.0f;

// ?BuildingMaxDist@@3MA
ARTS_EXPORT f32 BuildingMaxDist = 1000.0f;

f32 StaticTerrainLodTable[4][2] {
    {150.0f, 50.0f},
    {200.0f, 100.0f},
    {250.0f, 150.0f},
    {325.0f, 200.0f},
};

// ?LightDistances@@3PAMA
ARTS_EXPORT f32 LightDistances[4] {
    80.0f,
    160.0f,
    250.0f,
    350.0f,
};

void mmCellRenderer::Relight()
{}

// -nocull, the distance and detail half. See agiNoCullEnabled() (agiworld/meshrend.cpp) for the
// whole picture and for what this deliberately cannot reach.
//
// Every table touched here is a DISTANCE at which the engine reduces or drops something, and the
// consumers are almost all closed assembly - mmCellRenderer::Cull, asRenderWeb::Cull,
// mmInstance::Draw. Rewriting the thresholds rather than the code that reads them is what makes
// this reachable at all, and it works because every one of these comparisons is "is the object
// further away than X", so pushing X beyond any coordinate in the city answers no every time.
//
// 1e6 rather than FLT_MAX on purpose: some of these feed subtractions and squares in the closed
// consumers, and a value that overflows when squared would produce infinities instead of the
// intended "never". Chicago is a few thousand units across, so a million is already unreachable.
void mmApplyNoCullDistances()
{
    if (!agiNoCullEnabled())
        return;

    constexpr f32 kUnreachable = 1.0e6f;

    ObjectMaxDist = kUnreachable;
    BuildingMaxDist = kUnreachable;

    for (i32 quality = 0; quality < 4; ++quality)
    {
        for (i32 i = 0; i < 2; ++i)
            StaticTerrainLodTable[quality][i] = kUnreachable;

        LightDistances[quality] = kUnreachable;
    }

    // mmInstance::ComputeLod returns the FIRST level whose threshold the distance exceeds, and the
    // thresholds descend, so a table that can never be exceeded returns the highest detail level
    // for everything at any range. Rewriting the table rather than short-circuiting ComputeLod
    // keeps the closed consumers of the same table in step - the table is exported, so this code is
    // not the only reader.
    for (i32 type = 0; type < 3; ++type)
    {
        for (i32 quality = 0; quality < 4; ++quality)
        {
            for (i32 i = 0; i < 3; ++i)
                mmInstance::LodTable[type][quality][i] = kUnreachable;
        }
    }

    // The far clip proper. mmCullCity::Cull clamps fog range to this, and it bounds the projection,
    // so leaving it at AutoDetect's 1000 would keep discarding the far half of the city whatever the
    // LOD tables say.
    agiRQ.FarClip = kUnreachable;

    Displayf("NOCULL: backface, LOD and distance culling disabled. Portal/cell visibility is closed "
             "assembly and still applies.");
}
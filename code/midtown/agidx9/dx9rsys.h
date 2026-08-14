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

#include "agi/rsys.h"

#include "dx9pipe.h"

class Matrix34;
class agiViewParameters;
struct IDirect3DTexture9;

// Per-frame tally of how geometry reaches the device. "World" submissions are model-space vertices
// plus real SetTransform(WORLD/VIEW/PROJECTION) calls - the only kind RTX Remix can reconstruct a
// 3D scene from. "Screen" submissions are CPU-pretransformed XYZRHW vertices, which carry no
// world-space information at all. Reset and reported once per frame by agiDX9Pipeline::EndFrame().
struct agiDX9SubmitCensus
{
    u32 WorldCalls;
    u32 WorldTris;
    u32 WorldStaticLitTris;
    u32 WorldUnlitTris;

    // Split by render phase. Screen-space geometry submitted *inside* BeginScene/EndScene is real
    // 3D world content still going out CPU-pretransformed - that is the number that must reach
    // zero. Screen-space geometry submitted after EndScene is HUD, text, and the minimap, which
    // are genuinely 2D and are supposed to stay pretransformed.
    u32 ScreenCalls;
    u32 ScreenTris;
    u32 ScreenTrisInScene;
    u32 ScreenCallsInScene;

    u32 ScreenLineCalls;
    u32 ScreenLines;

    // -ffperpixel. Additive per-fragment sun passes submitted this frame; two per lit world draw
    // when the specular pass is on, one when it is not, zero when the path is off.
    u32 PerPixelPasses;

    // -ghash. Zero unless the switch is on. See agiDX9GHashRecord in dx9rsys.cpp for what is
    // hashed and why the churn number is the one that matters.
    u32 GHashDraws;
    u32 GHashStable;
    u32 GHashNew;
    u32 GHashDistinct;
};

extern agiDX9SubmitCensus agiDX9Census;

// Advances the -ghash frame counter. Called once per frame from agiDX9Pipeline::EndFrame, next to
// the census reset, because "was this hash also seen last frame" needs a frame number to compare.
void agiDX9GHashNextFrame();

class agiDX9Rasterizer final : public agiRasterizer
{
public:
    agiDX9Rasterizer(agiPipeline* pipe);
    ~agiDX9Rasterizer();

    void EndGfx() override;
    i32 BeginGfx() override;

    void BeginGroup() override;
    void EndGroup() override;

    void Verts(agiVtxType type, agiVtx* vertices, i32 vertex_count) override;
    void Points(agiVtxType type, agiVtx* vertices, i32 vertex_count) override;
    void SetVertCount(i32 vertex_count) override;
    void Triangle(i32 i0, i32 i1, i32 i2) override;
    void Line(i32 i0, i32 i1) override;
    void Card(i32 i0, i32 i1) override;
    void Mesh(agiVtxType type, agiVtx* vertices, i32 vertex_count, u16* indices, i32 index_count) override;

    bool MeshWorld(agiWorldVtx* vertices, i32 vertex_count, u16* indices, i32 index_count, const Matrix34& world,
        const Matrix34& view, const agiViewParameters& proj_params, bool static_lighting,
        const agiNativeMaterialFx* fx = nullptr, bool hardware_lighting = true) override;

    agiDX9Pipeline* Pipe() const
    {
        return static_cast<agiDX9Pipeline*>(agiRefreshable::Pipe());
    }

private:
    void FlushState();

    // Restores the device state MeshWorld() programs. Shared by both rendering pathways.
    void RestoreStateAfterWorldDraw(bool remap_vertex_fog);

    u16* ImmAddIndices(u32 prim_type, u16 count);
    void ImmDraw();

    void DrawMesh(u32 prim_type, agiVtx* vertices, i32 vertex_count, u16* indices, i32 index_count);

    IDirect3DTexture9* current_texture_ {};

    agiTexEnv tex_env_ {};
};

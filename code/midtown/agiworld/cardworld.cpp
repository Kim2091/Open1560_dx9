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

define_dummy_symbol(agiworld_cardworld);

#include "cardworld.h"

#include "agi/pipeline.h"
#include "agi/rsys.h"
#include "agi/vertex.h"
#include "agi/viewport.h"
#include "agiworld/meshset.h"

#include <algorithm>
#include <cstring>
#include <utility>

u32 agiWorldQuadsDrawn = 0;
u32 agiWorldQuadsDropped = 0;

// Sized for a dense night street: a few hundred glow flares, a few hundred smoke and spray
// particles, plus whatever the sparks are doing. Overflow drops the quad rather than growing the
// buffer, because this runs inside the draw loop and must not allocate - the same rule
// agiMeshSet::DrawNativeTransform documents at length for its own scratch.
inline constexpr u32 AGI_MAX_WORLD_QUADS = 4096;

namespace
{

    struct agiWorldQuad
    {
        Matrix34 World;
        Vector2 Corners[4];
        Vector2 UVs[4];
        agiTexDef* Texture;
        u32 Color;
        f32 Depth;
        i32 CornerCount;
    };

    agiWorldQuad s_Quads[AGI_MAX_WORLD_QUADS];
    u16 s_Order[AGI_MAX_WORLD_QUADS];
    u32 s_QuadCount = 0;

    // The view the queued quads were submitted under.
    //
    // Not incidental bookkeeping: BuildProjectionMatrix() reads agiViewParameters AND the
    // agiMeshSet::FlipX static, and FlipX is raised and lowered around the rear-view mirror pass. A
    // quad queued inside the mirror and flushed under the main view's state would come out mirrored
    // the wrong way, in the wrong rectangle. The flush restores both.
    //
    // In practice one snapshot is enough, because agiTexSorter::Cull() - which flushes this queue -
    // already runs per pass, so the queue never spans two views. SameView() below is the guard that
    // makes that assumption fail loudly (by flushing early) rather than silently.
    //
    // Held as raw storage rather than as an agiViewParameters object because agiViewParameters has
    // an imported constructor, and a namespace-scope instance of it would run that constructor
    // during CRT static initialisation for no benefit: the snapshot is overwritten in full before
    // anything reads it (see agiQueueWorldQuad - the copy happens when the queue is empty, and
    // SameView() is only consulted when it is not). The struct is plain data, so the copy is a
    // straight assignment once it is live.
    alignas(agiViewParameters) u8 s_ViewBytes[sizeof(agiViewParameters)] {};
    b32 s_ViewFlipX = false;

    agiViewParameters& SnapshotView()
    {
        return *reinterpret_cast<agiViewParameters*>(s_ViewBytes);
    }

    bool SameView(const agiViewParameters& params, b32 flip_x)
    {
        if (flip_x != s_ViewFlipX)
            return false;

        // View plus the four projection scalars BuildProjectionMatrix() actually consumes. Deliberately
        // not a memcmp of the whole struct: World and ModelView live in there too and change on every
        // SetWorld(), which would report a new view for every instance in the frame.
        return (std::memcmp(&params.View, &SnapshotView().View, sizeof(Matrix34)) == 0) &&
            (params.ProjX == SnapshotView().ProjX) && (params.ProjY == SnapshotView().ProjY) &&
            (params.ProjZZ == SnapshotView().ProjZZ) && (params.ProjZW == SnapshotView().ProjZW);
    }

} // namespace

bool agiWorldQuadsSupported()
{
    return Pipe()->SupportsNativeTransform() && !agiCurState.GetSoftwareRendering();
}

void agiResetWorldQuadStats()
{
    agiWorldQuadsDrawn = 0;
    agiWorldQuadsDropped = 0;
}

void agiQueueWorldQuad(agiTexDef* texture, const Matrix34& world, const Vector2* corners, const Vector2* uvs,
    i32 corner_count, u32 color, f32 view_depth)
{
    const agiViewParameters& params = ViewParams();

    if (s_QuadCount && !SameView(params, agiMeshSet::FlipX))
        agiFlushWorldQuads();

    if (s_QuadCount == 0)
    {
        SnapshotView() = params;
        s_ViewFlipX = agiMeshSet::FlipX;
    }

    if (s_QuadCount >= AGI_MAX_WORLD_QUADS)
    {
        ++agiWorldQuadsDropped;
        return;
    }

    agiWorldQuad& quad = s_Quads[s_QuadCount++];

    quad.World = world;
    quad.Texture = texture;
    quad.Color = color;
    quad.Depth = view_depth;
    quad.CornerCount = corner_count;

    for (i32 i = 0; i < corner_count; ++i)
    {
        quad.Corners[i] = corners[i];
        quad.UVs[i] = uvs[i];
    }
}

void agiFlushWorldQuads()
{
    if (s_QuadCount == 0)
        return;

    // The rasteriser can go away between a queue and a flush - EndGfx() drops it, and a pipeline
    // restart (the 640x480 menu into a 1280x800 race) runs a full EndGfx/BeginGfx cycle. Dropping
    // the queue is the right answer there: it describes a frame that is no longer being rendered.
    if (!RAST)
    {
        s_QuadCount = 0;
        return;
    }

    // Taken and cleared up front. MeshWorld() cannot reach DrawCard(), so this is not defending
    // against real reentrancy - it is defending against a future caller adding one and getting an
    // infinite flush instead of a bug report.
    const u32 count = std::exchange(s_QuadCount, 0u);

    for (u32 i = 0; i < count; ++i)
        s_Order[i] = static_cast<u16>(i);

    // Back to front. Additive glows do not care, but alpha-blended smoke and spray do, and the old
    // path could not sort at all - agiTexSorter groups by texture, so two overlapping smoke puffs
    // from different emitters composited in whatever order the traversal happened to reach them.
    std::sort(s_Order, s_Order + count, [](u16 lhs, u16 rhs) { return s_Quads[lhs].Depth > s_Quads[rhs].Depth; });

    // State the quads need for the duration, through agiCurState so MeshWorld()'s FlushState() picks
    // it up and agiLastState stays truthful - writing the device directly here would be the exact
    // cache-poisoning bug the MeshWorld() restore block exists to clean up after.
    //
    // ZWrite off: every one of these is a transparent overlay. A smoke quad that writes depth stakes
    // a claim on its whole rectangle and punches a hole in the particles behind it - the same
    // artifact MeshWorld() already documents for additive glows, which is why it forces the same
    // state for those.
    //
    // Cull None: a billboard is two-sided, and it also sidesteps the winding question entirely. The
    // world path rasterises with the opposite winding to the CPU path (see ToD3DCullFlipped), and
    // there is no natural front face for a camera-facing quad to have.
    const bool old_zwrite = agiCurState.SetZWrite(false);
    const agiCullMode old_cull = agiCurState.SetCullMode(agiCullMode::None);
    const agiBlendSet old_blend = agiCurState.SetBlendSet(agiBlendSet::SrcAlpha_InvSrcAlpha);
    agiTexDef* const old_texture = agiCurState.GetTexture();

    const b32 old_flip_x = agiMeshSet::FlipX;
    agiMeshSet::FlipX = s_ViewFlipX;

    // Card space is flat, so one normal serves every quad. Nothing reads it - these are submitted
    // unlit, exactly as the CPU path drew them, taking the card's own colour straight through - but
    // the FVF has the slot and D3D9 will read it whatever we put there.
    const Vector3 card_normal {0.0f, 0.0f, 1.0f};

    // Not const: agiRasterizer::MeshWorld takes a mutable index pointer, as every caller of it does,
    // and no implementation writes through it. A const_cast here would be the same thing with a
    // sharper edge.
    static u16 quad_indices[6] {0, 1, 2, 0, 2, 3};

    for (u32 i = 0; i < count; ++i)
    {
        const agiWorldQuad& quad = s_Quads[s_Order[i]];

        agiWorldVtx verts[4] {};

        for (i32 v = 0; v < quad.CornerCount; ++v)
        {
            verts[v].pos = Vector3 {quad.Corners[v].x, quad.Corners[v].y, 0.0f};
            verts[v].normal = card_normal;
            verts[v].color = quad.Color;
            verts[v].tu = quad.UVs[v].x;
            verts[v].tv = quad.UVs[v].y;
        }

        const i32 index_count = (quad.CornerCount == 4) ? 6 : 3;

        agiCurState.SetTexture(quad.Texture);

        // static_lighting and hardware_lighting both false: a card is emissive art, not a lit
        // surface. The colour in the vertex is the final colour, which is what the CPU path fed the
        // rasteriser too.
        if (RAST->MeshWorld(verts, quad.CornerCount, quad_indices, index_count, quad.World, SnapshotView().View,
                SnapshotView(), false, nullptr, false))
        {
            ++agiWorldQuadsDrawn;
        }
    }

    agiMeshSet::FlipX = old_flip_x;

    agiCurState.SetTexture(old_texture);
    agiCurState.SetBlendSet(old_blend);
    agiCurState.SetCullMode(old_cull);
    agiCurState.SetZWrite(old_zwrite);
}

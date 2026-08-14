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

define_dummy_symbol(agiworld_meshmodel);

#include "meshmodel.h"

#include "agi/pipeline.h"
#include "agi/rsys.h"
#include "agi/viewport.h"
#include "memory/alloca.h"
#include "skeleton.h"
#include "vector7/matrix34.h"

// agiworld/meshrend.cpp - the OPEN1560_NATIVE_MASK gate, shared so the model path can be bisected
// with the same switch as every other draw entry point.
bool agiNativePathEnabled(u32 which);

#define NATIVE_DRAWMODEL 0x10u

// Skins the model into `out` in MODEL space.
//
// This is agiMeshModel::ModelGeometry's transform step (game.asm ~339065) lifted out, and nothing
// more: pose the skeleton for this frame, then walk the vertex array in runs, transforming each run
// by its bone's matrix. The binding is rigid - one run of vertices per bone, no per-vertex weights -
// which is why a plain Vector3::Dot against a single Matrix34 is the whole of it.
//
// ModelGeometry itself then hands the result to agiMeshSet::Geometry, which projects and clips it
// on the CPU. That last step is exactly what has to be skipped to keep the geometry in world space,
// so the skinning is duplicated here rather than reusing the closed routine, and the closed routine
// is left untouched for the CPU fallback below.
// Whether the skinning inputs are self-consistent enough to run. Checked up front, every time,
// rather than assumed.
//
// The loop below writes `sum(SkinGroupVerts[0, SkinGroupCount))` vertices into a buffer sized
// VertexCount and reads that many from Vertices, and nothing in the format guarantees those agree -
// the assembly this was lifted from simply trusted them. A mismatch is not a wrong picture, it is a
// buffer overrun on a stack allocation, and its symptom appears somewhere else entirely and
// intermittently: the frame after, on a menu transition, in an unrelated draw. That is the most
// expensive class of bug to chase, so it is worth a few adds per pedestrian to make it impossible.
//
// Bones are checked too, because BoneMatrices is indexed by group and only holds
// Skeleton.BoneCount entries.
static bool CanSkinModel(agiMeshModel* model)
{
    const i32 groups = model->SkinGroupCount;
    const u8* run_lengths = model->SkinGroupVerts;

    if ((groups <= 0) || !run_lengths || !model->Vertices || !model->Skeleton.BoneMatrices)
        return false;

    if (groups > model->Skeleton.BoneCount)
        return false;

    u32 total = 0;

    for (i32 group = 0; group < groups; ++group)
        total += run_lengths[group];

    return total <= model->VertexCount;
}

static void SkinModelVertices(agiMeshModel* model, bnAnimation* anim, i32 frame, Vector3* ARTS_RESTRICT out)
{
    model->Skeleton.Pose(anim->Poses[frame]);
    model->Skeleton.Transform(nullptr);

    const Vector3* ARTS_RESTRICT src = model->Vertices;
    const Matrix34* bones = model->Skeleton.BoneMatrices;
    const u8* run_lengths = model->SkinGroupVerts;

    for (i32 group = 0; group < model->SkinGroupCount; ++group)
    {
        const Matrix34& bone = bones[group];

        for (u32 i = run_lengths[group]; i--;)
            (out++)->Dot(*src++, bone);
    }
}

// -pedskin. Restores CPU skinning for pedestrians, which is what the world path did before the
// rigid-group submission below. Off by default: skinned positions change every frame, so every
// pedestrian took a fresh RTX Remix geometry hash every frame and no replacement could stick to one.
static mem::cmd_param PARAM_ped_skin {"pedskin", "Skin pedestrians on the CPU (breaks RTX Remix hash stability)"};

// Whether every facet lies wholly inside one bone's vertex run.
//
// The rigid path submits one draw per bone and takes only the facets that bone owns outright, so a
// facet straddling two bones would simply not be drawn - a hole in the pedestrian rather than a
// visible seam. The models are built from rigid limb segments and in practice do not do this, but
// "in practice" is not something to bet a missing head on, so it is checked before committing and
// the CPU path is used unchanged when the check fails.
//
// Cheap enough to run per draw: a pedestrian is a few hundred facets, and this is four range tests
// each against a run table that is already in cache.
static bool ModelFacetsAreGroupCoherent(agiMeshModel* model)
{
    const u8* run_lengths = model->SkinGroupVerts;
    const i32 groups = model->SkinGroupCount;

    for (u32 facet = 0; facet < model->SurfaceCount; ++facet)
    {
        const u16* surface = &model->SurfaceIndices[facet * 4];
        const u32 corners = surface[3] ? 4u : 3u;

        u32 first = 0;
        i32 facet_group = -1;

        for (i32 group = 0; group < groups; ++group)
        {
            const u32 end = first + run_lengths[group];

            const u32 vertex = model->VertexIndices[surface[0]];

            if ((vertex >= first) && (vertex < end))
            {
                facet_group = group;
                break;
            }

            first = end;
        }

        if (facet_group < 0)
            return false;

        const u32 end = first + run_lengths[facet_group];

        for (u32 k = 1; k < corners; ++k)
        {
            const u32 vertex = model->VertexIndices[surface[k]];

            if ((vertex < first) || (vertex >= end))
                return false;
        }
    }

    return true;
}

// ?ModelDrawLit@agiMeshModel@@QAEHP6AXPAEPAI1PAVagiMeshSet@@@ZIPAVagiLitAnimation@@H@Z
//
// Pedestrians. This is the one draw entry point the world-space work never reached, because peds do
// not go through agiMeshSet::DrawLit at all - aiPedestrianInstance::Draw (game.asm ~94669) calls
// here instead, and this used to end unconditionally in FirstPass(), i.e. CPU-pretransformed screen
// triangles. That is why pedestrians were lit by nothing: invisible to RTX Remix as 3D geometry, and
// never seen by the programmable path either, so they stayed flat while the wall behind them
// responded to every street lamp.
//
// The animation makes no difference to that. Once the vertices are skinned they are ordinary
// model-space positions, and the frame's normals are ordinary packed normal indices - the same two
// inputs DrawNativeTransform() already takes. So the fix is to skin, point the mesh at the result,
// and submit, exactly as agiMeshSet::DrawLit does for a static mesh.
i32 agiMeshModel::ModelDrawLit(agiMeshLighter lighter, u32 flags, agiLitAnimation* anim, i32 frame)
{
    // Note on AGI_QUALITY_LOW: mmCullCity::fix_lighting() clears mmInstance::DynamicLighter
    // outright at that setting, so pedestrians arrive here with a null lighter and take ModelDraw()
    // below - the CPU path - however the native mask is set. That is faithful to the original (the
    // engine is being told not to light them at all), but it does mean the world-space path for
    // pedestrians is only reachable at MEDIUM and above.
    if (!lighter)
        return ModelDraw(flags, anim, frame);

    bnAnimation* animation = anim->Anim;

    // agiLitAnimation stores one packed normal per adjunct per frame, which is precisely the layout
    // of agiMeshSet::Normals - see the note on agiLitAnimation::Frames. Swapping it in for the
    // duration is what the original does for its lighter callback; the hardware path wants it for
    // the same reason, because it is the mesh's normal set for this frame.
    u8* saved_normals = Normals;
    u8* frame_normals = anim->Frames[frame];

    // The assembly tests +0x9C and only then indexes the +0xA0 table, so keep both: the flag is the
    // predicate, not the pointer.
    u32* base_colors = HasVariantColors ? VariantColors[MESH_DRAW_GET_VARIANT(flags)] : Colors;

    // Rigid-group submission - the hash-stable path, and the default.
    //
    // Nothing here is animated in the sense that matters to Remix. The binding is rigid (see
    // SkinModelVertices), so every vertex belongs to exactly one bone, and posing the model is a
    // per-bone transform rather than a per-vertex deformation. Handing that transform to
    // SetTransform(D3DTS_WORLD) instead of applying it to the vertices leaves the submitted
    // positions at their stored model-space values, so a pedestrian mesh hashes to the same thing
    // in every frame and every pose. That is the whole difference between a pedestrian Remix can
    // replace and one it sees as a new mesh sixty times a second.
    //
    // Normals come from the mesh's own bind-pose set, deliberately, not from the animation's
    // per-frame set. anim->Frames[frame] holds normals already posed for this frame; feeding those
    // to a draw whose world matrix also carries the bone would rotate them twice. Fixed-function
    // D3D9 transforms normals by the world matrix, so a bind-pose normal comes out posed for free -
    // and D3DRS_NORMALIZENORMALS is already on for world draws (see MeshWorld).
    //
    // Requires those bind-pose normals to exist. When they do not, the animation's set is the only
    // normal data there is and it only makes sense on CPU-skinned geometry, so that case takes the
    // skinning path below.
    if (Pipe()->SupportsNativeTransform() && CanSkinModel(this) && agiNativePathEnabled(NATIVE_DRAWMODEL) && Normals &&
        !PARAM_ped_skin.get_or(false) && ModelFacetsAreGroupCoherent(this))
    {
        Skeleton.Pose(animation->Poses[frame]);
        Skeleton.Transform(nullptr);

        const agiViewParameters& view_params = ViewParams();
        const Matrix34* bones = Skeleton.BoneMatrices;
        const u8* run_lengths = SkinGroupVerts;

        u32 first_vertex = 0;
        b32 any_drawn = false;

        for (i32 group = 0; group < SkinGroupCount; ++group)
        {
            agiNativeRigidGroup rigid {};

            // bone * world, in that order: the CPU path computes vertex * bone and the draw then
            // applies world, so the composed matrix has to reproduce (v * bone) * world.
            Matrix34 group_world;
            group_world.Dot(bones[group], view_params.World);

            rigid.World = &group_world;
            rigid.FirstVertex = first_vertex;
            rigid.EndVertex = first_vertex + run_lengths[group];

            first_vertex = rigid.EndVertex;

            // static_lighting = false for the same reason the skinned path gives: a pedestrian is a
            // mover, lit by the dynamic rig.
            any_drawn |= DrawNativeTransform(flags, /*static_lighting=*/false, nullptr, base_colors,
                /*unlit=*/false, &rigid);
        }

        if (any_drawn)
            return 1;

        // Every group empty means the mesh could not be expressed this way at all; fall through
        // rather than dropping the pedestrian, exactly as the skinned path does below.
    }

    if (Pipe()->SupportsNativeTransform() && frame_normals && CanSkinModel(this) &&
        agiNativePathEnabled(NATIVE_DRAWMODEL))
    {
        // ARTS_ALLOCA rather than a std::vector or the game's own allocator, both for the reason
        // documented in agiMeshSet::DrawNativeTransform - calling the engine allocator from inside a
        // draw corrupts the simulation heap - and because this is per-frame scratch with an obvious
        // lifetime. A pedestrian is a few hundred vertices; the ceiling is BigVtxSize.
        Vector3* skinned = ARTS_ALLOCA(Vector3, VertexCount);

        SkinModelVertices(this, animation, frame, skinned);

        Vector3* saved_vertices = Vertices;

        Vertices = skinned;
        Normals = frame_normals;

        // static_lighting = false: a pedestrian is a mover, lit by the dynamic rig, not by the
        // city's fixed sun/fill/fill. That is the same choice DrawLit() makes via
        // IsStaticCityLighter(), which never matches the ped lighter.
        const b32 drawn = DrawNativeTransform(flags, /*static_lighting=*/false, nullptr, base_colors);

        Vertices = saved_vertices;
        Normals = saved_normals;

        if (drawn)
            return 1;

        // Fall through to the CPU path rather than dropping the pedestrian. DrawNativeTransform
        // returns false for a mesh it cannot express, and a missing pedestrian is worse than an
        // unlit one.
    }

    // --- Original CPU path -----------------------------------------------------------------------
    // Unchanged in behaviour from the assembly this replaces: skin and project via the closed
    // ModelGeometry, light into a scratch buffer with the frame's normals swapped in, restore, and
    // hand the shaded colours to FirstPass.
    if (ModelGeometry(flags, animation, frame) > 0xFF)
        return 0;

    u32* shaded = ARTS_ALLOCA(u32, AdjunctCount);

    Normals = frame_normals;

    lighter(codes, shaded, base_colors, this);

    Normals = saved_normals;

    FirstPass(shaded, TexCoords, 0);

    return 1;
}

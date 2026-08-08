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

class bnBone;

struct bnSkeleton
{
public:
    // ??0bnSkeleton@@QAE@XZ
    ARTS_IMPORT bnSkeleton();

    // ?FindBone@bnSkeleton@@QAEPAVbnBone@@PADHD@Z
    ARTS_IMPORT bnBone* FindBone(char* arg1, i32 arg2, char arg3);

    // ?Load@bnSkeleton@@QAEHPBD@Z
    ARTS_IMPORT i32 Load(const char* arg1);

    // ?Pose@bnSkeleton@@QAEXPBVVector3@@@Z
    ARTS_IMPORT void Pose(const Vector3* arg1);

    // ?Transform@bnSkeleton@@QAEXPAVMatrix34@@@Z
    ARTS_IMPORT void Transform(Matrix34* arg1);

    // Read from bnSkeleton::Load (game.asm ~339729), the only writer of either: it stores the bone
    // count at +0x14, then allocates `BoneCount * 0x30` bytes - 0x30 being sizeof(Matrix34) - at
    // +0x1C and Identity()s every entry. The constructor (game.asm ~339720) zeroes +0x14 and +0x18,
    // which is the other half of the evidence. bnSkeleton::Transform() is what poses the matrices.
    //
    // offset_field rather than named members: this layout is frozen by the assembly, and a property
    // at a byte offset declares no storage, so it cannot perturb sizeof or shift a neighbour.
    offset_field(0x14, i32, BoneCount);

    // BoneCount entries, model space. Only valid after Pose()/Transform() for the current frame.
    offset_field(0x1C, Matrix34*, BoneMatrices);

    u8 gap0[0x20];
};

check_size(bnSkeleton, 0x20);

class bnAnimation
{
public:
    // ?Load@bnAnimation@@QAEHPAD@Z
    ARTS_IMPORT i32 Load(char* arg1);

    // From the agiLitAnimation constructor (game.asm ~339488), which reads [anim+0] as the frame
    // count to size its per-frame tables, and from agiMeshModel::ModelGeometry, which asserts
    // [anim+4] == skeleton bone count + 1 before posing. [anim+8] is the per-frame pose data
    // bnSkeleton::Pose() consumes.
    offset_field(0x00, i32, FrameCount);
    offset_field(0x04, i32, BoneCount);
    offset_field(0x08, Vector3**, Poses);

    u8 gap0[0xC];
};

check_size(bnAnimation, 0xC);

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

// World-space billboards and lines.
//
// Everything the engine draws as a flat quad or a line - tyre smoke, dust, debris, water spray,
// street-lamp and traffic-signal coronas, headlight/tail-light flares, collision sparks - reached
// the device CPU-pretransformed. agiMeshSet::DrawCard did its own ModelView multiply, its own
// frustum clip, its own perspective divide and its own viewport map, and handed the result to
// agiTexSorter as D3DFVF_XYZRHW screen triangles. agiMeshSet::DrawLines did the same for lines.
//
// Pretransformed vertices carry no world-space information, so RTX Remix cannot place them in the
// scene at all: the entire effects layer was missing from the reconstruction while the city, the
// cars and the pedestrians around it were present. This queue is what moves it over.
//
// Two design points are load bearing and should not be undone:
//
//   * The GEOMETRY IS CANONICAL AND THE MOTION IS IN THE MATRIX. A queued quad's four vertices are
//     the card's own corner offsets in "card space" - the same handful of values for every card
//     sharing a rotation index and animation frame - and its world position, size and orientation
//     live entirely in a per-quad Matrix34 handed to SetTransform(D3DTS_WORLD). Remix hashes vertex
//     positions, UVs and indices, and deliberately not the transform (see agiDX9GHashRecord), so
//     this produces a small fixed set of geometry hashes that are byte-identical frame after frame
//     and can be categorised or replaced. Building world-space vertices per frame instead would
//     work on screen and be useless to Remix, because every sprite would hash differently every
//     frame. -ghash reports the difference directly as CHURN.
//
//   * THE QUADS ARE DEFERRED, NOT DRAWN WHERE THEY ARE SUBMITTED. On the old path they went into
//     agiTexSorter and were flushed with the rest of the alpha content at the end of the pass.
//     Submitting them immediately instead would draw them during the cull traversal, and since a
//     transparent quad must not write depth, any opaque geometry drawn later in the traversal would
//     paint straight over it - smoke vanishing behind a building the moment the traversal reached
//     that cell. So they accumulate here and flush at exactly the point agiTexSorter::Cull() flushes
//     its own alpha sets.
//
// Depth sorting comes free with the deferral and is a small improvement on what the sorter did:
// back to front by view depth, rather than grouped by texture in submission order.

#include "vector7/matrix34.h"
#include "vector7/vector2.h"

class agiTexDef;

// True when quads should be queued rather than CPU-pretransformed: the pipeline supports the
// hardware-transform path and we are not in software rendering. The per-entry-point OPEN1560_NATIVE_MASK
// bit is checked by the caller, since cards and lines have their own bits.
bool agiWorldQuadsSupported();

// Queue one quad.
//
// `world` maps card space to world space, and is where the position, size and billboard orientation
// go. `corners` are the quad's card-space corner offsets (z is implicitly 0), `uvs` the matching
// texture coordinates; both must have `corner_count` (3 or 4) entries. `view_depth` is the
// positive view-space distance used to sort back to front.
//
// Both arrays are copied. Nothing is drawn until agiFlushWorldQuads().
void agiQueueWorldQuad(agiTexDef* texture, const Matrix34& world, const Vector2* corners, const Vector2* uvs,
    i32 corner_count, u32 color, f32 view_depth);

// Draw and empty the queue. Safe to call with an empty queue. Called from agiTexSorter::Cull(alpha)
// so the ordering matches what the sorter used to produce, and again from agiDX9Pipeline::EndScene()
// so nothing can escape the scene unflushed - a quad flushed after EndScene would be counted as HUD
// and, worse, would be drawn with the full-backbuffer viewport rather than its own.
void agiFlushWorldQuads();

// Reported by the DX9 census once every 120 frames. `Dropped` is the overflow count and should stay
// at zero; if it does not, AGI_MAX_WORLD_QUADS is too small for the scene.
extern u32 agiWorldQuadsDrawn;
extern u32 agiWorldQuadsDropped;

void agiResetWorldQuadStats();

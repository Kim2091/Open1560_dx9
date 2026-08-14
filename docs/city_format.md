# Open1560 — the city file format

What a Midtown Madness city is made of, field by field, and which parts of the loader are
reimplemented C++ versus still original assembly.

Everything here was read out of this repository: the open loaders in `code/midtown/mmcity`, and for
the closed ones, `code/midtown/game.asm`. Where something is inferred rather than decoded it says so.

---

## 0. The short version

A city is **not** one file. `mmCullCity::Init` (`mmcity/cullcity.cpp`) and the AI loader between
them pull in nine kinds of data:

| Data | File | Format | Loader | Reimplemented? |
|---|---|---|---|---|
| Cell table | `city/<name>.cells` | **plain text** | `asRenderWeb::LoadCells` | **yes** |
| Portals | `city/<name>.ptl` | binary | `asRenderWeb::LoadPortals` | **yes** |
| Geometry source | `geo/<name>city.geo`, `geo/<name>lm.geo` | **DLP container** | `DLPTemplate::Load` | **yes** |
| Geometry cache | `bms/<name>_<group>` | binary, **generated** | `agiMeshSet::BinaryLoad` | **yes** |
| Collision source | `geo/<mesh>.geo` | **DLP container** | `mmBoundTemplate::Load` | no — decoded below |
| Collision cache | `bnd/<mesh>_<group>` | binary, **generated** | `mmBoundTemplate::Load` | no |
| Props | `city/<name>.bng` | binary | `mmCullCity::LoadBangers` | no — decoded below |
| Facades | `city/<name>.fcd` | binary | `mmCullCity::LoadFacades` | no — decoded below |
| AI / roads | `city/<name>/<name>.map` + `<street>.road` | **text (MetaClass)** | `aiMap::Init` | no — decoded below |
| AI cache | `city/<name>/<name>.bai` | binary, **generated** | `aiMap::ReadBinary` | no |

Two things are worth knowing up front, because both invert how hard this looks. The visibility
structure — the part that sounds hardest — is the most open: `.cells` is plain text and `.ptl` has a
working writer. And the AI road network, which has no open loader, does not need one: `.bai` is a
**cache compiled from text sources**, and the game rebuilds it itself when it is stale (§7.1).

---

## 1. `city/<name>.cells` — the cell table

Plain text, parsed with `Gets`/`atoi`/`strtok` in `asRenderWeb::LoadCells`
(`mmcity/renderweb.cpp`). Authorable in a text editor.

```
<num_cells>
<max_cell_index>
<cell_index>,<cull_flags>[,<room_flags>,<tag_count>,<tag0>,<tag1>,...]
...
```

* Line 1 is how many cell records follow. Line 2 is the highest cell index used; the loader
  allocates `MaxCells = that + 1`.
* `cull_flags` is a **bitmask of which LOD groups this cell has** — fully decoded in §3.1.
* Everything after `cull_flags` is optional. When absent, `room_flags` and `tag_count` are 0.
* `tag_count` is asserted below 100 ("Too many visit tags").
* A cell index outside `1 .. MaxCells-1` is fatal: `Bad cell index CULL%02d`.

Each record becomes an `mmCellRenderer` plus an `asPortalCell` registered in `CellArray[cell_index]`.

### 1.1 The cell index also selects the mesh container

This is the non-obvious part. `CITY_MESH_START = 200`:

```cpp
const char* mesh_name = (cell_index >= CITY_MESH_START || !enable_lm) ? city_mesh : lm_mesh;
```

So a cell below 200 takes its geometry from `<name>lm`, and 200 or above from `<name>city`. The
index is not just an identifier — it decides which file the geometry is fetched from.

### 1.2 Index conventions

Documented in `mmcity/renderweb.h`:

```
[LM]   1-199 : Open Areas / Landmarks
       200   : Uppers
[CITY] 201-859 : Roads
       860+    : Intersections
```

**The city is partitioned into road segments and intersections.** That is why portal culling works
so well in a game with long sight lines down streets, and it is the single most useful fact here for
anyone generating a city: the partition is a road graph, so an OSM way segment maps to a cell and a
junction maps to an intersection cell.

Chicago additionally forces `PORTAL_CELL_EDGE_SORTING` on five hand-picked cells — Wrigley outer
(24) and inner (174), the Aquarium (31), the Planetarium (32) and McCormick (39).

---

## 2. `city/<name>.ptl` — portals

Binary. Header is two `i32`:

```
i32 vec_count     // asserted == 0 ("Unexpected PTL data")
i32 portal_count
```

Then `portal_count` records:

```c
struct PtlPortal {      // 0x24 bytes
    u8      Flags;      // 0x00  OR'd into asPortalEdge::Flags
    i8      EdgeCount;  // 0x01  must be 2 or 3
    i16     gap2;       // 0x02  padding
    i16     Cell1;      // 0x04
    i16     Cell2;      // 0x06
    f32     Height;     // 0x08
    Vector3 Min;        // 0x0C
    Vector3 Max;        // 0x18
};
```

**If `EdgeCount == 3`, one extra `Vector3` follows the record.** That is the only variable-length
part of the format, and missing it desynchronises everything after it.

### 2.1 How a portal polygon is built

The stored box is expanded into a 4- or 5-sided polygon (`NumEdges = EdgeCount + 2`):

```
EdgeCount 2  ->  (Min.x, Min.y+Height, Min.z), (Max.x, Max.y+Height, Max.z), Max, Min
EdgeCount 3  ->  (Min.x, Min.y+Height, Min.z), (Max.x, Max.y+Height, Max.z), Max, extra, Min
```

The plane is then computed from the first three vertices. So `Min`/`Max` are the two *bottom* corners
and `Height` lifts the top pair — a portal is a vertical quad spanning a doorway or street opening,
with the optional fifth vertex allowing a non-rectangular opening.

A portal referencing cell 0, a cell index past `MaxCells`, or a cell that does not exist is skipped
with a registered problem rather than being fatal — `Cell doesn't exist [missing open area?]` is the
message, and it names the missing cell as `CULL%02d`.

### 2.2 The engine can write this file

`asRenderWeb::SavePortals()` writes `city/optimized.ptl` in exactly the format above, and
`OptimizePortals()` runs first. In dev builds both are Bank buttons ("Optimize Portals", "Save
Portals"), alongside `DrawAllBounds()`.

`OptimizePortals` does two things worth knowing:

* **Planarity check.** Any portal vertex more than 0.1 units off its own plane disables the portal
  and logs `Edge %d/%d ... is not planar!`. Generated portals must be planar.
* **Plane orientation.** It probes `GetStartCell` on both sides of the plane, walking downwards in
  five 10-unit steps, to decide which side is `Cell1`. When it can determine this it sets
  `Flags_ForwardPlane` and, if the sides came out swapped, reverses the winding and negates the
  plane. Portals it cannot resolve still work but cost more at runtime.

It also warns (without disabling) when two connected cells are further apart than
`CellMagnitude1 + CellMagnitude2 + 5.0` — a cheap sanity check for a portal joining cells that have
no business touching.

`chicago.ptl` in `1560.ar` is already optimised, so the loader skips `OptimizePortals()` for it by
name. Any new city gets optimised on every load until you save the result.

---

## 3. `geo/<name>.geo` — the DLP container, and how cells use it

`.geo` is not a city-specific format. It is **the** geometry container for the whole game: city cell
meshes, bound sources and banger models all come out of one. Three separate loaders open
`geo/%s.geo` — `GetMeshSet`, `mmBoundTemplate::Load` and
`mmBangerDataManager::AddBangerDataEntry` — and all of them go through `GetDLPTemplate`.

**And unlike everything else in this document, it did not need decoding from the disassembly.**
`DLPTemplate::Load`, `DLPGroup::Load`, `DLPPatch::Load` and `DLPVertex::Load` are all reimplemented
C++ in `agi/dlptmpl.cpp`, with matching `Save` methods. What follows is read straight off them.

### 3.1 File layout

```
u32     Magic          // 'DLP7' == 0x444C5037, so the bytes on disk read "7PLD"
u32     NumGroups
u32     NumPatches
u32     NumVertices

NumGroups   x DLPGroup
NumPatches  x DLPPatch
NumVertices x Vector3        // the shared vertex-position pool

agiLib  MLib                 // materials
agiLib  TLib                 // textures
agiLib  PLib                 // physics / surface
```

A wrong magic is fatal: `Not a valid .dlp file, or old version.`

### 3.2 Groups are index lists, not meshes

```
u8      NameLen              // asserted <= 32
char    Name[NameLen]
u32     NumVertices
u32     NumPatches
u16     VertexIndices[NumVertices]
u16     PatchIndices[NumPatches]
```

This is the architectural point of the whole format. **A group owns no geometry.** It is a pair of
index lists selecting into the template's shared patch and vertex pools. `CULL01`, `BOUND01`,
`HITID`, `HOT_VERTS` are all *views* over the same data.

That is what lets one `.geo` hold an entire city: every cell names a group, groups overlap freely,
and a vertex shared between two cells is stored once. It is also why the bound groups (§4) live in
the same file as the visual geometry rather than beside it — a collision hull and the wall it
belongs to are two selections over one pool.

`DLPGroup::NumVertices` is documented in the header as "the set of all vertex indices used by
enabled patches in this group", so the vertex list is derived from the patch list rather than
independent.

### 3.3 Patches are parametric grids

```
u16     SRes
u16     TRes                 // NumVertices = SRes * TRes
u16     Flags                // Flags_Enabled is CLEARED on load, see below
u16     ROpts
u16     MtlIdx
u16     TexIdx
u16     PhysIdx
        SRes*TRes x DLPVertex
string  Name                 // Stream::GetString
```

A patch is an **SRes x TRes grid of vertices**, not a triangle list — which is what the name means
and why the resolution is stored rather than a vertex count. A flat quad is 2x2.

`Flags_Enabled` (`0x1`) is masked off as the patch loads (`Flags = Get<u16>() & ~Flags_Enabled`), so
whatever the file says about enablement is discarded and the runtime decides. `ROpts` is the render
options word, and its bits are named in `agi/dlptmpl.h`:

| Bit | Name | | Bit | Name |
|---|---|---|---|---|
| 0x001 | `CPV` | | 0x020 | `ZWrite` |
| 0x002 | `Emission` | | 0x040 | `ZRead` |
| 0x004 | `Shade` | | 0x080 | `Shadow` |
| 0x008 | `Solid` | | 0x100 | `Flat` |
| 0x010 | `Cull` | | 0x200 | `Antialias` |
| | | | 0x400 | `Interpenetrate` |

`MtlIdx`, `TexIdx` and `PhysIdx` are **1-based** indices into the three libraries below —
`agiLib::operator[]` asserts `index > 0 && index <= count`, and index 0 means "none".
`DLPTemplate::InitRemap` rewrites them through the `MtlIds` / `TexIds` / `PhysIds` remap tables at
load, binding them to the engine's global libraries rather than the file's local ones.

### 3.4 Vertices

```
u16     Id                   // index into the template's shared Vector3 pool
Vector3 Normal
Vector2 UV
u32     Color                // A8R8G8B8, expanded to a float Vector4 in memory
```

26 bytes on disk, `0x28` in memory (`Id` widens to `u32` and the colour to four floats). Note the
split: **position lives in the shared pool and is referenced by `Id`, while normal, UV and colour
are per-patch-vertex.** Two patches meeting at an edge share the position but carry their own
normals and UVs — which is exactly the adjunct/vertex split that reappears in `agiMeshSet`.

### 3.5 The three embedded libraries

Each is an `agiLib` (`agi/agilib.h`), a trivial container:

```
u32     Count
Count x <record>
```

with records:

**MLib — `agiMtlParameters`** (`0x68`): `char Name[32]`, then `Emmisive`, `Ambient`, `Diffuse` and
`Specular` as `Vector4`, then `f32 Power`.

**TLib — `agiTexParameters`**: `char Name[32]`, then `u8 Flags`, `u8 LOD`, `u8 MaxLOD` and one
padding byte. Flag bits: `Alpha 0x1`, `WrapU 0x2`, `WrapV 0x4`, `KeepLoaded 0x8`, `NoMipMaps 0x10`,
`Chromakey 0x40`, `Second 0x80`.

**PLib — `agiPhysParameters`**: `char Name[32]`, then `Friction`, `Elasticity`, `Drag`,
`BumpHeight`, `BumpWidth`, `BumpDepth`, `SinkDepth` as floats. This is the per-surface driving feel —
tarmac versus grass versus kerb — carried in the geometry file next to the polygons it applies to.

`agiTexParameters::Load`/`Save` are open C++ (`agi/texdef.cpp`); the material and physics ones are
still imported, but their member layouts are declared in `agi/mtldef.h` and `agi/physdef.h`.

### 3.6 `.geo` -> `.bms`: the fourth bake

`GetMeshSet` (`game.asm` ~336882) reads a group out of the `.geo` and writes an `agiMeshSet` cache to
`bms/<name>_<group>`, printing

```
Meshset %s.%s changed version or offset, recomputing
```

when the cache is stale. Along the way it welds vertices, remaps textures and builds adjacency — the
asserts it carries (`vtxrm[i] >= 0 && vtxrm[i] < vtxcount`, `texcount < 256`,
`NB[vi].Adj[k].PosI == vi`) still name its original source file, `C:\mm\src\agiworld\getmesh.c`.

So the chain is **`.geo` (authoring) -> `.bms` (mesh cache) and `.bnd` (bound cache)**, and both
caches regenerate. `agiMeshSet::BinaryLoad` and `BinarySave` are open C++ (`agiworld/meshload.cpp`,
`meshsave.cpp`), so `.bms` can be read and written directly too — which is the shortcut a generator
would take rather than emitting `.geo`.

### 3.7 Eight LOD slots, selected by `cull_flags`

`mmCellRenderer` holds `agiMeshSet* Meshes[8]` at offset `0x08`, and `Init` (`game.asm` ~189366)
fills them by `formatf`-ing a group name per slot and calling

```cpp
GetMeshSet(container, group, /*offset*/ nullptr, /*flags*/ 7);
```

`7` is `MESH_SET_UV | MESH_SET_NORMAL | MESH_SET_CPV`, so **cell geometry always loads with normals
and vertex colours** — unlike instance meshes, which only get them when
`mmInstance::InitMeshes` decides they are a collider, mover or obstacle.

Which slots are attempted comes bit by bit from **`cull_flags`, the second column of `.cells`**:

| `cull_flags` bit | Group name | Slot |
|---|---|---|
| 0 (`0x001`) | `CULL%02d_A` | `Meshes[3]` |
| 1 (`0x002`) | `CULL%02d_L` | `Meshes[0]` |
| 2 (`0x004`) | `CULL%02d_M` | `Meshes[1]` |
| 3 (`0x008`) | `CULL%02d_H` | `Meshes[2]` |
| 5 (`0x020`) | `CULL%02d_A2` | `Meshes[7]` |
| 6 (`0x040`) | `CULL%02d_L2` | `Meshes[4]` |
| 7 (`0x080`) | `CULL%02d_M2` | `Meshes[5]` |
| 8 (`0x100`) | `CULL%02d_H2` | `Meshes[6]` |

So the array is ordered **[L, M, H, A, L2, M2, H2, A2]** — increasing detail, then the `A` variant,
then the same four again. Bit 4 is unused; the low five bits are tested together (`and eax, 0x1F`)
as "does this cell have any first-set geometry at all".

**Bit 3 is a switch, not merely a presence flag.** Set, the high LOD is looked up as `CULL%02d_H`;
clear, the *same slot* is filled from the unsuffixed `CULL%02d`. A cell either uses suffixed LOD
naming or a single plain group. If neither resolves, the load aborts with
`Group CULL%02d (or _H) is missing from city '%s'`.

`Init` then logs `Flags nlod=%d h=%d m=%d l=%d a=%d h2=%d m2=%d l2=%d a2=%d`, runs `GetPolyInfo` over
all eight slots, appends a `%d,%d,%d,%d,%d,%d,%d,%d,%d` row to `<name>_static.csv` (§8), and derives
`CellCenter` and `CellMagnitude` via `GetBoundInfo` — the two values `OptimizePortals` uses for its
suspicious-portal distance check (§2.2).

### 3.8 The rest of `mmCellRenderer`

`Init(container, index, cull_flags, room_flags, tag_count, tags)` maps one-to-one onto the `.cells`
columns. Beyond the mesh slots it stores `Index`, `RoomFlags`, `VisitTagCount` and `VisitTags`
directly; `cull_flags` is the only column not kept, because it has already been consumed.

`asRenderWeb::PassMask` splits drawing into `RENDER_PASS_TERRAIN` (roads, grass, water, bridges),
`SHADOWS`, `BUILDINGS`, `OBJECTS` and `LIGHTS`. The `…2` slots line up with the second pass, given
the `MULTIPASS` global next door in `renderweb.h` — consistent, but not proven.

---

## 4. Collision bounds — `.geo` and `.bnd`

`LoadHitId` prefers a dedicated `<name>_hitid` bound and falls back to the `BOUND` group of
`<name>city`, recording which it got in `HasHitIdBound`. `LoadRoomBounds` then loads one bound per
cell, named `BOUND%02d`, from the same container the geometry came from — **but only when
`HasHitIdBound` is true**, so a city with no `_hitid` silently gets no per-room collision at all.

Chicago overrides two: cell 60 (construction) uses `dl60_bnd`, both Wrigley cells use `dl24_bnd`.

### 4.1 `GetBoundTemplate` is a cache in front of `Load`

Decoded from `game.asm` ~350772. It builds a key with `sprintf("%s_%s", name, group)`, looks it up
in a global `HashTable`, and returns the existing template with an `AddRef` on a hit. On a miss it
allocates `0xB4` bytes, calls `mmBoundTemplate::Load`, and on success `strdup`s the key into
`Name` (offset `0xB0`) and inserts it. A failed `Load` deletes the object and returns null.

### 4.2 `.bnd` is generated from `.geo`

`mmBoundTemplate::Load` (`game.asm` 342715..344747) works on two paths:

* **source** — `geo/%s.geo`
* **cache** — `bnd/%s`, opened `"r"` to read and `"w"` to write

It calls `OutOfDate()` on the pair, and when the cache is stale, or its header does not match, it
prints

```
Bound file '%s/%s' format or offset doesn't match, regenerating.
```

and rebuilds from the `.geo`. **This is the same source-and-cache arrangement as `.map` -> `.bai`
(§7.1) and the portal optimisation (§2.2), and it means `.bnd` files never need to be authored
either** — ship the `.geo` and let the game bake them.

The group names it looks for inside a `.geo` are `CULL`, `HITID` and `HOT_VERTS`. A missing one
gives `mmBoundTemplate::Load: no group '%s' in '%s'`.

An assert in this function still carries its original source path, `C:\mm\src\mmdyna\bndtmpl2.c`.

### 4.3 `.bnd` header and scalar block

Header, in file order:

```c
u32     Magic;    // 0x424E4432 - 'B','N','D','2' as a dword, so the bytes on disk read "2DNB"
Vector3 Offset;   // the origin this bound was baked at
i32     XDim;     // -> mmBoundTemplate +0x80
b32     YDim;     //                    +0x84
i32     ZDim;     //                    +0x88
```

Both the magic **and** the offset are validated: the stored `Offset` is differenced against the
`Vector3*` the caller passed to `GetBoundTemplate` and the squared distance tested. A cache baked at
a different origin is rejected and regenerated, which is why the offset is part of the header rather
than applied at load.

The scalar block then reads straight into the object, and every target maps onto a named member of
`mmBoundTemplate` (`mmdyna/bndtmpl.h`):

| Size | Offset | Field |
|---|---|---|
| 0x0C | 0x1C | `Center` |
| 0x04 | 0x28 | `Radius` |
| 0x04 | 0x2C | `RadiusSqr` |
| 0x0C | 0x30 | `BBMin` |
| 0x0C | 0x3C | `BBMax` |
| 0x04 | 0x48 | `NumVerts` |
| 0x04 | 0x4C | `NumPolys` |
| 0x04 | 0x58 | `NumHotVerts1` |
| 0x04 | 0x5C | `NumHotVerts2` |
| 0x04 | 0x60 | `NumEdges` |
| 0x04 | 0xA0 | `XScale` |
| 0x04 | 0xA4 | `ZScale` |
| 0x04 | *static* | `mmBoundTemplate::NumIndexs` — a **static**, not a member |
| 0x04 | 0x9C | `HeightScale` |
| 0x04 | *stack* | one more count, consumed locally |

After that, `EnableBinaryFileMapping` decides whether the bulk arrays (`Verts`, `Polygons`,
`HotVerts`, `EdgeVerts1/2`, `RowOffsets`, `BucketOffsets`, `RowBuckets`, `FixedHeights`) are
memory-mapped in place or read into freshly allocated blocks — hence the `Tell`/`Seek` pairs around
that point. Those array layouts are **not decoded here**; the member declarations in `bndtmpl.h`,
including the bucket-walk comment above `RowOffsets`, describe how they are used at runtime.

`XDim`/`YDim`/`ZDim` with `XScale`/`ZScale`/`HeightScale` and the `RowOffsets` / `BucketOffsets` /
`RowBuckets` triple are a **uniform grid index over the polygons** — the bound is a heightfield-ish
spatial hash, not a raw mesh, which is why it is baked rather than loaded directly from `.geo`.

---

## 5. `city/<name>.bng` — bangers (props, street furniture)

Decoded from `game.asm` ~176659. Opened as `city/%s.bng`, mode `"r"`.

```
i32 count
count x {
    u16     CellIndex;    // 0x00
    u16     Flags;        // 0x02
    Vector3 Position;     // 0x04
    Vector3 Orientation;  // 0x10   (inferred - see below)
                          // 0x1C total
    char    Name[];       // NUL-terminated, immediately follows the record
}
```

The record is a fixed 28 bytes followed by a variable-length name read byte-by-byte until NUL, so
records are not evenly spaced.

Each becomes `mmCullCity::AddInstance(CellIndex, Name, nullptr, Flags, &Position, &Orientation,
nullptr, 0.0f)`. The loader prints `***** %d bangers in city`.

Field meanings were resolved by following `AddInstance` (`game.asm` ~176207):

* **Argument 1 is the cell index** — it is passed as the second parameter of
  `mmInstChain::Parent(mmInstance*, i16)`, which is what files the instance into a cell's chain.
* **Argument 4 is a flags word** — `test bl, 1` on entry decides whether the instance is registered
  with `mmBangerDataManager::AddBangerDataEntry`.
* The second `Vector3` is passed where a rotation would go; that it is an orientation rather than a
  second position is **inferred from position, not proven**.

### 5.1 Bangers are loaded more than once, per game mode

`mmCullCity::Init` loads the base `<name>.bng` and then one overlay depending on the mode:

| Mode | Extra file |
|---|---|
| Cruise | `<name>_roam.bng` |
| Checkpoint | `<name>_r<EventId>.bng` |
| Cops and Robbers | `<name>_cops.bng` |
| Circuit | `<name>_c<EventId>.bng` |
| Blitz | `<name>_b<EventId>.bng` |

This is how checkpoints, barriers and race dressing appear only in the event that needs them. A
generated city needs at minimum the base file; the overlays may be empty but each race references one.

---

## 6. `city/<name>.fcd` — facades

Decoded from `game.asm` ~176779. Opened as `city/%s.fcd`, mode `"r"`. Same shape as `.bng` but a
larger record:

```
i32 count
count x {
    u16     CellIndex;   // 0x00
    u16     Flags;       // 0x02
    Vector3 Position;    // 0x04
    Vector3 AxisA;       // 0x10   (inferred)
    Vector3 AxisB;       // 0x1C   (inferred)
    f32     Scale;       // 0x28   (inferred)
                         // 0x2C total
    char    Name[];      // NUL-terminated
}
```

It reaches the same `AddInstance`, this time filling all three `Vector3*` parameters and the trailing
float. Position plus two vectors plus a scalar is the natural description of a flat wall segment —
an origin and two edge directions — which is what a facade is, but the exact roles of `AxisA`,
`AxisB` and `Scale` are **inferred from the call shape and not confirmed**.

---

## 7. The AI road network

Loaded from `mmgame/game.cpp` (~line 820), not from `mmCullCity`:

```cpp
arts_sprintf(city_folder, "city/%s", MapName);
FindFile(MapName, city_folder, ".map", ...) || FindFile(MapName, city_folder, ".bai", ...)
AIMAP.Init(RaceDir, aimap_file, city_folder, &Player->Car);
```

The AI data lives in a **per-city subfolder** `city/<name>/`, unlike the flat files above.

### 7.1 `.bai` is a build artifact, not a source file

This is the thing worth knowing before anything else. Decoded from `aiMap::Init`
(`game.asm` ~57176):

1. The `.map` path is copied and `strrchr(path, '.')` locates its extension, which is overwritten
   with `.bai`. The two names share a stem — `.bai` is derived from `.map`, not independent.
2. `OutOfDate(bai_path, map_path)` compares timestamps.
3. **In `DevelopmentMode`, if the `.bai` is stale the whole network is rebuilt from text**, and then
   `aiMap::SaveBinary` writes a fresh `.bai`.
4. Otherwise `aiMap::ReadBinary` loads the cache and none of the text is touched.

So a new city never needs a hand-authored `.bai`. Ship `.map` and `.road`, run once in development
mode, and the game compiles the binary itself. That removes what looked like the hard blocker.

### 7.2 The text sources are MetaClass files

`mmMapData` and `mmRoadSect` both derive from `mmInfoBase`, whose `Load`/`Save` are open C++
(`mmcityinfo/infobase.cpp`) and run through `StreamMiniParser` plus the engine's reflection system:

```cpp
GetClass()->Load(&parser, this);   // and Save(), symmetric
```

The schema is therefore whatever `DeclareFields` declares, the files are text, and **they
round-trip: the same code that reads them can write them.**

**`<name>.map` — `mmMapData`** (size `0x90`), two fields:

| Field | Meaning |
|---|---|
| `NumStreets` | count |
| `Street` | array of street names, `NumStreets` entries |

`aiMap::Init` walks it with `GetNumItems()` / `GetItem(i)` and, for each name, calls
`FindFile(street, city_folder, ".road", …)`.

**`<street>.road` — `mmRoadSect`** (size `0x6CC`). Field names and offsets read out of
`mmRoadSect::DeclareFields` (`game.asm` ~264366):

| Offset | Field | Notes |
|---|---|---|
| 0x650 | `NumVertexs` | also the element count for `Normals` and `RoomIds` |
| 0x654 | `NumLanes[0]` | lanes in direction 0 |
| 0x658 | `NumLanes[1]` | lanes in direction 1 |
| 0x65C | `NumSidewalks[0]` | |
| 0x660 | `NumSidewalks[1]` | |
| 0x664 | `TotalVertexs` | element count for `Vertexs` |
| 0x668 | `Vertexs` | array, `TotalVertexs` entries |
| 0x66C | `Normals` | array, `NumVertexs` entries |
| 0x670 | `RoomIds` | array, `NumVertexs` entries — ties road vertices to cells |
| 0x674 | `IntersectionType[0]` | see `GetIntersectionType()` |
| 0x678 | `IntersectionType[1]` | |
| 0x67C | `StopLightPos[0]` | `Vector3` |
| 0x688 | `StopLightPos[1]` | `Vector3` |
| 0x694 | `StopLightPos[2]` | `Vector3` |
| 0x6A0 | `StopLightPos[3]` | `Vector3` |
| 0x6AC | `Blocked[0]` | see `IsBlocked()` |
| 0x6B0 | `Blocked[1]` | |
| 0x6B4 | `PedBlocked[0]` | see `IsPedBlocked()` |
| 0x6B8 | `PedBlocked[1]` | |
| 0x6BC | `StopLightName` | see `GetStopLightName()` |
| 0x6C4 | `Divided` | see `IsDivided()` |
| 0x6C8 | `Alley` | see `IsAlley()` |

The four `StopLightPos` entries are 12 bytes apart, confirming `Vector3` — one per approach to an
intersection. Almost everything is a per-direction pair, which is what makes a street two-way.

`Vertexs`, `Normals` and `RoomIds` are declared with a separate count-offset argument, which is why
they take more pushes in the disassembly than the scalar fields do.

`RoomIds` is the link back to §1: road vertices carry the cell they belong to, so the AI network and
the visibility partition share a coordinate system.

### 7.3 Roadside props are data too

`mmRoadSide` (size `0x2E0`) declares six fields, each an `mmPropInfo`:

```
Sidewalk   Curb   Bldgs   Signs   Trees   Posts
```

and `mmPropInfo` (size `0x94`) declares:

| Field | Meaning |
|---|---|
| `Spacing` | distance between placements |
| `NumThings` | count |
| `Things` | array of object names |

So street dressing is **procedurally distributed along the road from a spacing plus a name list**,
not hand-positioned the way `.bng` bangers are. That is a generator-friendly format: supply a
palette and a spacing and the engine does the placement.

### 7.4 How the graph is built

Per street, `aiMap::Init` does:

```
AddAIPath(roadsect, 0, n)          // one directed path per side
AddAIPath(roadsect, 1, n)
GetVertex(...) at each end   ->    AddIntersection(Vector3*)
AddSourcePath / AddSinkPath        on both intersections
CalcCenterVerts(roadsect, dir)     per path
```

So **a street becomes two directed paths, and each endpoint becomes or joins an intersection that
records its source and sink paths.** That is a directed road graph — the same shape an OSM extract
already has, which is what makes generating one plausible.

Both `AddAIPath` calls receive the same third argument, read from offset `0x654` (`NumLanes[0]`),
rather than the per-direction lane count one might expect. Observed, not explained; treat that
parameter's meaning as open.

After all streets are processed, `CreateAmbAppRoadMap` and `CreatePedAppRoadMap` build the ambient
and pedestrian approach-road tables (memory section "AI APP ROAD MAP"), and only then does
`SaveBinary` run — so those tables are part of the cache, not recomputed at load.

### 7.5 Race data

Selected by game mode from the strings in `aiMap::Init`: `race%d`, `roam`, `circuit%d`, `blitz%d`
(memory section "AI RACE DATA"), loaded relative to `RaceDir`. This sets opponent counts and routes;
`aiMap::NumOpponents` is an `i16` and `mmgame` asserts it is at most 8.

### 7.6 What is still unread

`aiMap::ReadBinary` (`game.asm` 59281..59876) and `aiMap::SaveBinary` have not been decoded field by
field — and largely do not need to be, since the text sources are authorable and the binary
regenerates from them. Anyone who does want that layout should start there, and can check the work by
round-tripping a known city and diffing the result.

---

## 8. Other files

* `<name>_static.csv` — *written*, not read. `mmCullCity::Init` opens it for writing as a log.
* `city/optimized.ptl` — written by `SavePortals`, see §2.2.
* Texture sets are selected by suffix rather than by city: `TextureSuffix` is `_fall` in rain and
  `_win` in snow (`cullcity.cpp`), appended in `agiTexSorter` (`agiworld/texsort.cpp`), and
  `TEXSHEET.SetUseAlternate()` switches an alternate sheet at sunset and night.
* Time of day and weather presets live in `mmEnvSetup[4][4]` (`cullcity.cpp`), indexed
  `[TimeOfDay][Weather]`, each entry naming a sky, a vehicle sphere map and a ground shadow map plus
  fog and light values. This is plain exported C++ data, not a file.

---

## 9. What this means for generating a city

Ordered by how much of the work is already done for you.

**Already writable from open code.** Mesh data via `agiMeshSet::BinarySave`
(`agiworld/meshsave.cpp`), portals via `SavePortals`, and `.cells` is text. `.bng` and `.fcd` are
simple enough to emit from the layouts above.

**Four formats bake themselves.** `.bms` from `.geo` (§3.6), `.bnd` from `.geo` (§4.2), `.bai` from
`.map`/`.road` (§7.1), and an optimised `.ptl` from a rough one (§2.2). All four compare timestamps,
regenerate when stale and write the result. None needs a hand-authored binary — which is most of
what once looked like the hard part of this format.

**Everything upstream of those bakes is either text or an open C++ format.** `.geo` is the one
container the whole game is built out of, and `DLPTemplate::Load`/`Save` are reimplemented, so a
generator can emit `.geo` and let the engine derive meshes and bounds — or skip a step and write
`.bms` directly through `agiMeshSet::BinarySave`.

**Constraints a generator must respect.**

* Cell indices below 200 come from `<name>lm`, 200 and above from `<name>city`. Pick the split
  before generating geometry, not after.
* Every cell needs `CULL%02d` or `CULL%02d_H` in its container or the load fails loudly.
* Portals must be planar to within 0.1 units, or `OptimizePortals` disables them.
* Per-room bounds are only loaded when a `_hitid` bound exists. Skipping it silently costs you all
  per-cell collision.
* Each game mode expects its own banger overlay file.

**The AI network is authorable after all.** `.map` and `.road` are text driven by declared fields
(§7.2), and running once in development mode compiles the `.bai` for you. Roadside props come free
from a spacing and a name list (§7.3). This was the blocker in an earlier draft of this document; it
is not one.

**Natural mapping from open data.** The engine's own partition is a road graph — segments and
intersections — so OSM ways become cells, junctions become intersection cells, and the portal
between two connected segments is their shared road cross-section. LoD1/LoD2 building footprints
match the engine's fidelity closely enough that little is lost.

---

## 10. How the formats above were read

Check `code/midtown` before reaching for `game.asm`. The `.geo` container looked like the deepest
closed format in the game and turned out to be fully reimplemented in `agi/dlptmpl.cpp`, with
`Save` alongside `Load`; the same is true of `.cells`, `.ptl`, `.bms` and the `mmInfoBase` text
files. Time spent disassembling any of those would have been wasted.

For the ones that genuinely are closed: each loader was located in `code/midtown/game.asm` by its
mangled name, then read for three things: the format string handed to `sprintf` (which gives the
filename pattern), the size passed to `Stream::Read` (which gives the record size), and the argument
order at the call it forwards to (which gives the field layout). String constants resolve by grepping
their `asc_` label in the same file.

Field *meanings*, as opposed to offsets and sizes, generally need the consumer to be read as well —
`AddInstance` is what proved argument 1 is a cell index, by following it to `mmInstChain::Parent`.
Anything not established that way is marked inferred above, and should be treated as a hypothesis.

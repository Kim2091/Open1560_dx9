# Open1560 — DirectX 9 renderer handoff

Everything a newcomer needs to work on the D3D9 backend: how the project is put together, how to
build it quickly and cleanly, what the renderer actually does, and where the programmable path is
going next.

No source excerpts here on purpose — this is the map, not the territory. File and symbol names are
given so you can find the code, and `docs/dx9_rendering_pathways.md` holds the design rationale.

---

## 1. What this project is

Open1560 is a partial re-implementation of Midtown Madness 1 (Sneak Preview beta, build 1560). It is
**not** a from-scratch engine and not a decompilation you can read end to end. It is a hybrid:

* Functions that have been reimplemented live in C++ under `code/midtown`.
* Everything not yet reimplemented is still the original x86, assembled from `code/midtown/game.asm`
  and linked into the same binary.

The seam between the two is the single most important thing to internalise, because almost every
surprising bug in the renderer comes from it.

### The ARTS_IMPORT / ARTS_EXPORT seam

* `ARTS_EXPORT` marks a symbol that C++ *provides* and the assembly may call.
* `ARTS_IMPORT` marks a symbol that still lives in assembly and C++ calls into.

Consequences you must respect:

* **Class layouts are frozen.** Anything the assembly touches has its field offsets baked into the
  original machine code. `check_size` assertions enforce this. You cannot add a member to
  `agiMeshSet`, `agiRendState`, or similar — if you need per-object state, use a side table or a
  global, both of which have precedent in the codebase.
* **Virtual table slots are frozen.** New virtuals must be appended *after* every original one, or
  the assembly's hardcoded vtable offsets break. `agiRasterizer::MeshWorld` and
  `agiPipeline::SupportsNativeTransform` are both appended for exactly this reason.
* **Access level is part of the linked symbol name.** MSVC encodes private/protected/public into
  C++ mangling, so widening the access of an `ARTS_IMPORT` static changes the symbol you link
  against and the build fails. Add an accessor instead. `mmSky::IsFlashing` exists because of this.
* **Closed code has invisible expectations.** Several imported routines read scratch state that an
  earlier C++ call is expected to have populated. `agiMeshSet::SphereMap` and `EnvMap` read the
  leftovers of a CPU `Geometry()` pass; calling them without one aborts with "Bug?". When a new path
  bypasses the old one, audit what the old one left behind.

### Renderer layers, bottom to top

* `agi` — device-independent rendering interfaces: `agiPipeline`, `agiRasterizer`, `agiTexDef`,
  render state (`agiCurState` / `agiLastState`), viewport and view parameters.
* `agiworld` — mesh representation and submission: `agiMeshSet` (the game's mesh type), its draw
  entry points, lighting rigs (`agiMeshLighter*`), texture sheets, normal packing.
* `agirend` — shared software-ish helpers: lighters, DLP, the Z-buffer renderer.
* `agigl`, `agisw`, `agisdl`, `agidx9` — concrete backends. `agidx9` is ours.
* `mmcity`, `mmcar`, `mmai`, … — game code. It calls `agiMeshSet` draw entry points; it does not
  know which backend is active.

### Two ways geometry reaches the GPU

This is the central fact about the D3D9 backend.

**The CPU pretransform path (original).** `agiMeshSet::Geometry` transforms and clips vertices on the
CPU, `FirstPass` hands screen-space vertices to the rasterizer, and they are submitted as
pretransformed (XYZRHW) triangles. Correct on screen, but such a draw carries **no world-space
information** — nothing downstream can reconstruct a 3D scene from it.

**The world-space path (added).** `agiMeshSet::DrawNativeTransform` submits model-space vertices plus
real world/view/projection matrices, through `agiRasterizer::MeshWorld`. This is what NVIDIA RTX
Remix needs, and it is also the only path the programmable shaders run on.

A per-frame **census** prints to the log every 120 frames showing how much of the frame went out each
way, plus the live glow-light count. "world share (3D only)" is the number to watch — it should stay
high. In-scene screen triangles are real 3D content still going out pretransformed; post-scene ones
are HUD, text and minimap, which are meant to be 2D.

`OPEN1560_NATIVE_MASK` (an environment variable, bitmask, default all) selects which draw entry
points may take the world path. Setting it to 0 restores original behaviour with no rebuild — the
fastest way to answer "is this regression mine?".

---

## 2. Building — clean and fast

### The command

The toolchain is not on `PATH` and cannot be auto-detected. `build/tmp/dobuild.bat` sets `PATH`,
`INCLUDE`, `LIB` and `TEMP` explicitly and is self-contained. Run it, redirect output to a log, then
grep the log for `MSBUILD_EXIT` and `Error(s)`. Success is `MSBUILD_EXIT=0` with `0 Error(s)`.

### Rules that actually save time

1. **Kill the running game before building.** A live `Open1560.exe` holds the deployed binary and the
   post-build copy fails *after* a successful compile, which is maximally annoying. `taskkill` it
   first, every time.

2. **Verify the deploy, do not assume it.** The post-build copy can fail silently and leave you
   testing a stale binary — this has cost real debugging time. Compare timestamps between the build
   output and the deployed copy, and copy manually if they differ.

3. **Build in the background and wait for the notification.** A full rebuild is around seven minutes.
   Do not poll in a loop.

4. **Touching a header triggers a near-full rebuild.** The PCH invalidates and almost everything
   recompiles. Batch header changes together; several separate header edits cost several full
   rebuilds. Editing only `.cpp` files is dramatically faster.

5. **Shaders need no rebuild at all.** HLSL is compiled at runtime from loose files, so editing a
   shader and relaunching is a seconds-long loop. Use this — it is by far the fastest way to iterate
   on anything visual, and it is why runtime compilation was chosen over shipping bytecode.

6. **You can compile-check shaders without launching.** `fxc.exe` ships in the Windows SDK `bin`
   directory. Compile both pixel-shader permutations (with and without the `LIT` define) before
   running; a shader that fails to compile silently falls back to fixed function, which looks like a
   rendering bug rather than a build error.

7. **`/W4 /WX` — warnings are errors.** Two recurring offenders: deprecated CRT functions
   (`getenv`, `fopen`) need a local pragma suppression, and unused variables left behind after
   removing a code path.

8. **`cmd_param` must be declared at namespace scope.** A function-local `static mem::cmd_param`
   constructs lazily on first call — long after argument parsing has already distributed values — so
   it registers too late and silently never receives one. This has bitten twice. If a setting
   mysteriously has no effect, check this first.

### Environment notes specific to this machine

* The `C:` drive has been full. Shell pipes that buffer through `C:` temp can fail intermittently.
  Builds are unaffected because `dobuild.bat` redirects `TEMP` to `D:`.
* The deploy target is an external SSD over USB that occasionally drops out under load. The symptom
  is a build failing with a page-fault exit code and no output. It recovers — retry rather than
  investigating hardware.

---

## 3. How the D3D9 renderer is organised

Everything is under `code/midtown/agidx9`.

* **`dx9pipe`** — the `agiPipeline` implementation. Owns the device context, frame begin/end, the
  census, and brings up the shader system. Declares that this backend supports the world path.
* **`dx9context`** — device creation, depth format selection, device-lost handling, present.
* **`dx9rsys`** — the rasterizer. Contains both the screen-space submission path and `MeshWorld`,
  which is where the world path forks between fixed-function and programmable.
* **`dx9texdef`** — texture upload, sampler state, and the glow colour grid used by the lighting.
* **`dx9view`**, **`dx9bitmap`** — viewport and 2D blit support.
* **`dx9shader`** — the programmable path: runtime HLSL compilation, vertex declaration, material
  resolution, light gathering, and constant upload.
* **`dx9config`** — loads `Open1560-Shaders.ini`.

Shaders live in `game/hlsl` and are deployed alongside the game data.

### Configuration

`Open1560-Shaders.ini` sits next to the executable and is **written automatically on first run**,
fully commented, with every value at its current default. It is not a separate settings system: each
key is an existing command-line parameter applied through the same mechanism, so there is one source
of truth for defaults and anything tunable on the command line is tunable from the file.

The file loads *before* command-line parsing, so **the command line overrides the file** — the file
is a persistent preference, the command line a deliberate one-off. Unknown keys are ignored safely;
malformed lines warn.

---

## 4. The two rendering pathways

Full rationale in `docs/dx9_rendering_pathways.md`. The short version:

**Pathway A — fixed function.** The default. Reproduces the original shading model using D3D9's
fixed-function transform and lighting. This is the path RTX Remix can reconstruct a scene from,
because Remix understands fixed-function material and light state but cannot know what a vertex
shader did to a position. If you are capturing for Remix, this is the path you want.

**Pathway B — programmable.** Opt-in. `vs_3_0` / `ps_3_0`, per-pixel Cook-Torrance lighting, PBR
material response, per-pixel fog, tonemapping, and dynamic lights harvested from the game's own
light-flare sprites. Free to diverge from the original because nothing downstream reinterprets it.

Failure is always soft: no shader compiler, insufficient hardware, or a shader that fails to compile
all fall back to Pathway A with a log line. Nothing about the default configuration depends on the
programmable path existing.

### Where PBR material data comes from

MM1 ships colour maps only — no roughness, metalness or normal maps anywhere. Rather than invent
values, material parameters are derived from `agiTexProp`, the engine's own per-texture semantic
flags: road/floor surfaces, worn surfaces, transparent surfaces, alpha-keyed foliage, and content
explicitly marked as emissive or not-to-be-lit. That is real artist intent rather than guesswork, and
it needs no new assets.

### Glow-driven lighting

The city is full of things that look like light sources but which the original engine draws as
additive billboards emitting nothing. Those draws carry a world position, a colour and a size — so
the draw stream itself is the light database, requiring no new assets and no hand placement.

Two harvest points are needed because glows arrive by two routes: billboards (street lamps, traffic
signals) and glow *meshes* (all vehicle lighting — head, tail, brake, reverse). Covering only
billboards misses every vehicle light in the game.

Light colour is sampled from the flare texture itself, on a coarse grid rather than a single average,
because one texture routinely holds several differently coloured lamps — a traffic signal sheet
carries red, amber and green side by side, selected by UV sub-rect. Sampling where the flare actually
reads is what makes signals cycle correctly. The sampling is luminance-weighted, because a glow sheet
is mostly black background by design and a flat mean would drag every light toward black.

Intensity is classified per kind — a street lamp exists to light a street, a tail light exists to be
seen — with each multiplier exposed in the ini rather than baked in. The test is on *relative*
saturation and is re-run by the renderer against the colour sampled out of the flare texture, not
just the card's vertex tint. Both of those matter: an absolute channel-spread test classified every
warm-white street lamp as a traffic signal (warm white has a wide absolute spread by definition) and
classified any faded saturated lamp as a street light, so the two populations were swapped in both
directions depending on how brightly a flare happened to be drawn.

### Clustered point lighting

Up to **256** simultaneous point lights, looked up per pixel from a wrapping world-space grid of
32x8x32 cells holding 16 light indices each. Both the light data and the cell table are textures,
sampled with `tex2Dlod` — which is the escape from `ps_3_0` having no relative addressing for pixel
shader constants, and the reason the light loop can be a real loop instead of an unrolled one.

The practical consequence is that there is **no per-draw light work at all**. The pool and the grid
are built once per frame in `BeginFrame`; the pixel shader finds its own lights from its world
position. The previous design chose sixteen lights per *draw* with a sort over the whole live set,
repeated for every mesh — which is what cost the most with a street full of traffic.

`d3d9cellsize` tunes the grid, `d3d9lightspec` turns off point-light specular on weaker hardware.
Full rationale, including why the grid is world-space and wrapping rather than a view-space froxel
grid, is in `docs/dx9_rendering_pathways.md` §B0c.

---

## 5. Hard-won lessons

These cost real time. Read them before assuming something is broken.

**A position is not a position until you know its space.** `agiMeshSet::DrawCard` takes a *model*-space
position and projects it through `ModelView`. Half its callers set an identity world matrix first, so
for those, model space and world space coincide and any code that reads the position raw appears to
work. The other half do not, and street lighting was broken for exactly that reason for a long time —
every lamp registering its light at the raw `GlowOffset` near the world origin while its sprite drew
in the correct place. When harvesting anything out of a draw call, transform by the matrix that draw
is itself using.

**Diagnostics must not deduplicate on the wrong key.** `glowdebug` originally logged one line per
texture. Street lamps and traffic signals share the `FXLTGLOW` sheet, a signal always drew first, and
so no street lamp ever printed a line — making "lamps are never harvested" and "lamps are harvested
and then dropped" look identical in the log. Two wrong diagnoses came out of that before the key was
widened to texture + hue. If a log is being used to rule something out, check what it cannot show.

**Measure, do not eyeball.** Several wrong diagnoses in this work came from judging renders visually.
Reading exact pixel values from a diagnostic render settled in one step what hours of looking had got
wrong. When lighting looks wrong, render the quantity you suspect — the normal, the light direction,
the sign of a dot product, the lighting term with albedo divided out — and sample the pixels
numerically.

**Test in the right venue.** The vehicle-select showroom never loads the city, so the sun rig is
never initialised and sits on the horizon with no vertical component. Anything lighting-related
tested there is meaningless. Use gameplay.

**Suspect the plumbing before the algorithm.** Every "the lighting model must be wrong" moment turned
out to be an integration bug: an energy convention, a discarded vertex attribute, an inverted facing
test, a selection step throwing away the light before it reached the shader. The BRDF was never the
problem.

**Data quality bounds shading quality.** Mesh normals are stored as an index into a 198-entry
direction table, roughly 26 degrees apart. That is coarse enough that all three corners of a facet
often quantise to the *same* normal, and interpolating a constant gives a constant — so low-poly
bodywork shades flat no matter how per-pixel the lighting is. No shading model can recover a gradient
from constant input; the normals have to be rebuilt.

**Beware state the cache does not know about.** The render state cache only re-issues a value when it
*changes*. Any code that writes the device directly and does not restore it leaves the cache lying,
and the symptom appears in some unrelated later draw. This has produced black boxes around glows,
depth-write leaks, sampler address-mode leaks and double-fogged geometry. Restore what you set.

**The world path's winding is reversed** relative to D3D9's convention, because the view matrix
carries a reflection. Culling compensates for this explicitly, and the same reversal inverts the
pixel shader's facing test. Anything that keys off triangle orientation needs the same treatment.

**Remix path traces nothing past the first "UI" draw of a frame — and this engine manufactures fake
UI.** dxvk-remix (`d3d9_rtx.cpp`, `makeDrawCallType`) classifies a fixed-function draw as UI when
the current projection looks orthographic (`_44 == 1.0f`; identity qualifies) and
`D3DRS_ZWRITEENABLE` is off, gated on `rtx.orthographicIsUI` (default true). The first UI draw
latches RTX injection; every draw after it that frame is rasterized only, never path traced — and
the game still renders normally, because `rtx.skipDrawCallsPostRTXInjection` defaults false. Every
in-scene screen-space effect here (blob shadows, glow cards, smoke) is RHW with zwrite off, and
`RestoreStateAfterWorldDraw` resetting all three transforms to identity left exactly the
orthographic-looking projection that makes Remix call them UI — so one blob shadow ended path
tracing for the rest of the frame. Under the Remix bridge (detected by DLL name in `CreateD3D9`)
the perspective projection is now left in place; RHW draws ignore transforms per the D3D9 spec.
RHW alone never latches — `rtx.preTransformedVerticesIsUI` defaults false. The zero-build way to
confirm the mechanism: `rtx.orthographicIsUI = False` in `rtx.conf`.

---

## 6. Where to go next

Roughly in dependency order.

### Infrastructure (unblocks everything else)

1. **A single owner for device state**, with scoped save/restore. This retires an entire recurring
   bug class rather than fixing instances of it, and every item below is safer afterwards.
2. **Real vertex and index buffers.** Every draw currently goes through the slowest submission path
   D3D9 offers, and re-uploads a mesh's whole vertex array once per texture batch. Static city
   geometry can go further and cache immutable buffers per mesh, removing the per-frame CPU vertex
   build entirely.
3. **Frustum culling on the world path.** Currently absent, so meshes are fully built and submitted
   whether or not they are on screen.
4. **Render targets.** Nothing exists for this yet, and it gates shadows, post-processing and proper
   image-based lighting. This is the highest-leverage piece of missing infrastructure.
5. **Device-loss handling** must release default-pool resources before a reset. Mandatory once (2)
   lands.

### Programmable path

6. **Shadow mapping.** The single largest remaining visual gain — both sun shadows during the day and
   shadows from the glow lights at night. Needs (4). One cascade covering the near field is enough
   for gameplay; the existing blob-shadow path can remain as the far-field fallback.
7. ~~**More lights per draw than the current unrolled budget allows.**~~ *Done* — clustered lighting,
   256 lights, see above. What is left of it: the pixel shader is at 454 of the 512 `ps_3_0`
   instruction slots, so anything substantial added to the lit permutation from here will need
   something taken out, or a further permutation split.
8. **Spot/cone shaping for lamps.** Street lamps should throw downward rather than radiate. The
   engine records no orientation for a billboard, so this needs either a height-based heuristic or a
   small authored table.
9. **Normal mapping.** The plumbing choice is already made — deriving a tangent frame from screen
   derivatives avoids changing the vertex format or the mesh loader, both of which are constrained by
   the frozen layouts. It needs authored normal maps to be worth anything.
10. **Post-processing.** Render to a floating-point target, then bloom (the game is full of emissive
    glows that currently clamp), filmic tonemapping in a proper pass rather than inline, and optional
    antialiasing. Needs (4).
11. **Environment probes** for real image-based lighting, replacing the current two-lobe hemisphere
    approximation. Would also let the grazing-angle specular damping on rough surfaces be removed in
    favour of something physical.

### Pathway A parity work

12. Restore the environment and sphere-map second passes, which the world path currently drops.
13. Give the reduced-quality lighting rig its own hardware mapping instead of routing it to the full
    rig, so the light-quality setting means something again.

---

## 7. Diagnostics available

* **Submission census** — printed every 120 frames. World vs screen triangle split, static-lit vs
  unlit counts, and the live glow-light count.
* **`OPEN1560_NATIVE_MASK`** — disable the world path per draw entry point, no rebuild. The fastest
  way to bisect a rendering regression.
* **`glowdebug`** — logs each glow texture the first time it is harvested, with position, radius,
  tint and UV. This is what established that street lamps were being harvested correctly and
  discarded later, rather than never being picked up.
* **Runtime shader editing** — change HLSL, relaunch, see the result. Temporarily returning a
  diagnostic value from the pixel shader (a normal, a light direction, a dot product) and sampling
  the resulting pixels is the most reliable debugging technique available here.

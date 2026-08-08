# Open1560 — Two DirectX 9 rendering pathways

Design proposal for `code/midtown/agidx9`. Written against the current state of the world-space
(`agiRasterizer::MeshWorld` / `agiMeshSet::DrawNativeTransform`) path.

---

## 0. Why two, and what each is for

The DX9 backend today has one pathway and it is being asked to serve two goals that pull in
opposite directions:

1. **Be reconstructable by RTX Remix.** Remix rebuilds a 3D scene from D3D9 draw calls. It needs
   model-space vertices plus real `SetTransform(WORLD/VIEW/PROJECTION)` calls, and it understands
   *fixed-function* material state (texture stage ops, `D3DMATERIAL9`, `D3DLIGHT9`). A draw issued
   through a vertex shader is opaque to it — Remix cannot know what the shader did to the position,
   so the geometry either falls out of the capture or is treated as a pass-through overlay. **This
   goal forbids shaders on world geometry.**
2. **Look better than 1999.** PBR materials, per-pixel lighting, normal mapping, proper specular,
   HDR-ish tonemapping. **This goal requires shaders.**

These cannot be the same code path. The proposal is therefore two *sibling* rasterizer backends
behind the existing `agiRasterizer` interface, selected once at `BeginGfx()` time, sharing
everything below the draw-submission layer.

| | **Pathway A — Fixed Function** | **Pathway B — Programmable** |
|---|---|---|
| Target | RTX Remix / path tracing, old GPUs | Native "remastered" look |
| Shader model | none (FF9 T&L) | `vs_3_0` / `ps_3_0` |
| Lighting | 8 `D3DLIGHT9` slots, per-vertex | per-pixel, 256 lights via clustering |
| Materials | `D3DMATERIAL9` + 1–2 texture stages | PBR: albedo / normal / ORM / emissive |
| Fog | table fog | analytic, in-shader, height-fog capable |
| Remix-safe | **yes** | no (and doesn't need to be) |
| Selector | `-d3d9` (default) | `-d3d9 -shaders` |

Pathway A is the *correctness* baseline: everything the engine can express, expressed exactly.
Pathway B is free to diverge, because nothing downstream is trying to reinterpret it.

---

## 1. Shared foundation (build this first — both pathways need it)

Neither pathway is worth building on the current submission layer. Three pieces come first.

### 1.1 `agiDX9StateCache` — single owner of device state

Today device state has two owners that do not agree: `FlushState()` writes through the
`agiCurState`/`agiLastState` pair, and `MeshWorld()` writes the device directly and then hand-rolls
a restore. Every bug in the "state leak" family in `dx9rsys.cpp` — the alpha-glow black boxes, the
`D3DRS_ZWRITEENABLE` leak out of the reflection pass, the sampler address-mode leak — is the same
bug: *someone changed the device without telling the cache, and the cache only re-issues on change.*

Replace both with one object that owns every `SetRenderState`/`SetTextureStageState`/
`SetSamplerState` call:

```cpp
class agiDX9StateCache
{
public:
    void Set(D3DRENDERSTATETYPE state, DWORD value);   // no-op if unchanged
    void SetSampler(DWORD stage, D3DSAMPLERSTATETYPE, DWORD value);
    void SetStage(DWORD stage, D3DTEXTURESTAGESTATETYPE, DWORD value);
    void SetTexture(DWORD stage, IDirect3DTexture9*);

    ScopedStateBlock Push();   // RAII: records what changed, restores it on scope exit
};
```

`ScopedStateBlock` is what kills the whole bug class. `MeshWorld()` becomes:

```cpp
auto scope = cache.Push();
cache.Set(D3DRS_LIGHTING, TRUE);
...draw...
// restore is automatic, exact, and cannot be forgotten
```

No `agiLastState` poisoning, no hand-written restore block, no "guarded because `Reset()` fills
with 0xFF".

### 1.2 Real vertex and index buffers

Every draw in the backend is `DrawIndexedPrimitiveUP`. That is the slowest submission path D3D9
offers — the driver copies and re-validates the whole vertex array on every call. It is also
charged per *texture batch*: `DrawNativeTransform()` re-submits the mesh's entire `AdjunctCount`
vertex array once per texture on the mesh, so a 4-texture building uploads its vertices four times.

Replace with a ring of dynamic buffers:

```cpp
class agiDX9DynamicBuffers
{
    IDirect3DVertexBuffer9* vb_;   // e.g. 4 MB, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY
    IDirect3DIndexBuffer9*  ib_;
    // D3DLOCK_NOOVERWRITE while appending, D3DLOCK_DISCARD on wrap
};
```

The mesh's vertices are written **once**, then each texture batch is a `DrawIndexedPrimitive`
against a different index range. This also removes the `thread_local` scratch vectors in
`DrawNativeTransform()`.

Static city geometry is a further win: `agiMeshSet` contents are immutable between loads, so cache
a `D3DPOOL_MANAGED` VB/IB pair on the `agiMeshSet` itself, keyed on the `agiMeshSet::Variant`, and
skip the per-frame CPU vertex build entirely. That build — `AdjunctCount` iterations of
`UnpackNormal[]` lookups and `Vector3` copies — is currently paid every frame for every visible
mesh.

### 1.3 Frustum culling on the world path

`DrawNativeTransform()` deliberately does no CPU-side cull, with the reasoning that the shared
`agiMeshSet::M` matrix might lag behind `SetWorld()`. Whatever the truth of that, the fix is not to
skip culling — it is to cull with the matrices *this path itself uses*, which are self-consistent by
construction:

```cpp
// ViewParams().World/View + BuildProjectionMatrix() — the same three matrices handed to
// SetTransform() moments later. Test BoundingBox's 8 corners; reject only when every corner
// is outside the same plane AND every corner has w > 0.
```

The existing `agiViewParameters::SphereVisible()` (already `ARTS_IMPORT`, takes centre + radius, and
`agiMeshSet` carries `Radius`/`BoundingBoxRadius`) is a cheaper first gate and should be tried
before the 8-corner test.

Without this, every mesh the culler hands down gets a full CPU vertex build and a draw call whether
or not it is on screen.

---

## 2. Pathway A — Fixed Function

**Goal: bit-faithful reproduction of the original shading model, expressed entirely in D3D9
fixed-function state, so RTX Remix sees a scene it can reconstruct.**

Nothing here is a visual improvement. Every item is "the FF pipeline can express this and we
currently don't".

**Status:** A0, A1 (partial), A2, A4, A5, A6 implemented and verified in-game. A1's
`agiMeshLighterQuarter` mapping, A3, A7 and all of §1 are outstanding.

### A0. Backface culling — the one that mattered

Not in the first draft of this document, and the single largest source of visible artifacts.

`MeshWorld()` forced `D3DCULL_NONE`, on the stated reasoning that `DrawNativeTransform()` had
already culled backfacing facets on the CPU using plane equations. Both halves are false:

- `agiMeshSet::IsBackfacing()` opens with `return AllowEyeBackfacing && …`. `AllowEyeBackfacing`
  initialises to **false** (`agiworld/meshrend.cpp`) and is raised only by `agiMeshSet::InitMtx`
  (game.asm ~324504), and only when *all* of: the caller passed `planes && SurfaceCount > 1`;
  `MirrorMode` is clear; and the transform passes InitMtx's scale tolerance. Every other path sets
  it straight back to 0 (~324569). For a large share of draws, nothing is culled at all.
- The CPU path never depended on that test alone. Its screen-space triangles go through
  `FlushState()`, which programs `D3DRS_CULLMODE` from `agiCurState` — `agiRasterizer`'s constructor
  sets `agiCullMode::CCW` (`agi/rsys.cpp`) — so the GPU culled by winding as a second line of
  defence. `D3DCULL_NONE` removed that line and left the world path drawing every back face.

That single state accounts for all four reported artifacts: rear/interior building walls
rasterising over their own front faces (**black triangle spots**, and the **"overlapping UV"** look,
which is a back face showing its texture mirrored), and coplanar road surfaces fighting their own
reverse side (**road z-fighting**).

**The winding is inverted relative to the CPU path** and must be compensated — see
`ToD3DCullFlipped()`. `MeshWorld`'s `view_zflip` negates the view matrix's Z *column*, a
single-axis reflection with negative determinant, so it reverses triangle orientation; the CPU path
reaches the same depth convention by negating `agiMeshSet::M`'s Z *row* and picks up a second
reflection from `InitViewport`'s `HalfHeight = -h/2`. Identical pixel positions, opposite winding.
Verified directly: passing `agiCurState`'s cull mode through unmodified culled *front* faces, and
the frame came back showing the inside of the city with the FINISH banner mirrored.

### A1. Split the two lighting rigs properly

The engine has two unrelated light sources and the backend already knows it (`static_lighting`).
Finish the mapping:

- **Static rig** (`agiMeshLighterTriple/Quarter`, city geometry): three directional lights +
  ambient, with ambient sourced from the vertex colour (`D3DMCS_COLOR1`) so it lands *inside* the
  product the CPU rig computes — `colour * (key + fill1 + fill2 + ambient)`, not
  `colour * (key + fill1 + fill2) + ambient`.
- **`agiMeshLighterQuarter` is not `Triple`.** It is what `AGI_QUALITY_MEDIUM`/`HIGH` select, and
  routing it to the full three-light rig means the light-quality setting no longer does anything on
  DX9. Either give it its own reduced rig or, better, drop the graphics option's DX9 entry to
  "hardware lighting" since the cost argument (CPU per-vertex lighting) no longer applies.
- **`agiConeLighter`** (the `-conelighter` debug rig) has no directional analogue at all; leave it
  on the CPU path rather than silently substituting `Triple`. *(Done — `CanNativeLight()`.)*
- **`D3DRS_NORMALIZENORMALS`** must be on for world draws. D3D9 FF does not renormalise after the
  world/view transform, and `agiWorldVtx::normal` is a unit vector in *model* space, so any scaled
  instance hands the lighting pipeline a non-unit normal and reads uniformly too bright or too dark.
  Scaled instances demonstrably exist — InitMtx's `AllowEyeBackfacing` guard is a scale-tolerance
  test, which would be pointless otherwise. *(Done.)*

### A2. Specular: opt-in, not on by default

`SetupD3D9StaticMaterial()` sets `Specular = 0.30, Power = 20` and enables
`D3DRS_SPECULARENABLE`. The original static rig has **no specular term whatsoever**
(`mmxTriple` computes `key + fill1 + fill2 + amb` and stops). This is a deliberate embellishment
sitting in the middle of the parity path, and it changes the look of every building in the city.

Move it behind `mem::cmd_param PARAM_d3d9_specular` (default off in Pathway A, default on in
Pathway B, where per-pixel specular replaces it anyway).

### A3. Multi-texture: env map and sphere map back on the world path

`DrawLitEnv()` and `DrawLitSph()` currently *drop* their second pass on the native path, because
`EnvMap()`/`MultiTexEnvMap()`/`SphereMap()` are closed `ARTS_IMPORT` routines that consume the CPU
`Geometry()` scratch state. That is a real visual downgrade — road sheen and vehicle chrome are gone.

Both are expressible in FF9 without touching the imported routines:

- **Sphere map** — the UV generation is already reimplemented in
  `BuildVehicleReflectionVertices()`. Promote it from the ad-hoc `agiNativeMaterialFx` second pass
  to a proper stage-1 setup with `D3DTSS_TEXCOORDINDEX = D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR`
  and `D3DTTFF_COUNT2 | D3DTTFF_PROJECTED`, so the GPU generates the coordinates and the pass
  becomes single-draw, two-stage instead of two draws.
- **Env map** — same mechanism with `D3DTSS_TCI_CAMERASPACENORMAL`.

`agiCurState.GetMaxTextures()` is pinned to 1 in `agiDX9Pipeline::BeginGfx()`; raise it to
`min(caps.MaxSimultaneousTextures, 2)` once stage 1 is programmed.

### A4. Retire the depth-bias hack by unifying depth

`kDepthBias = -0.00015f` exists because the CPU pretransform path and the hardware path compute the
same depth by two different routes and disagree in the last bits. It is a symptom, and it is applied
globally, so it also nudges geometry that had no conflict.

The real fix is to have nothing left on the CPU path inside the 3D scene. The census
(`agiDX9Census.ScreenTrisInScene`) already measures exactly this — the remaining in-scene screen
triangles are the work list. When that reaches zero, delete the bias.

*Done, as far as it can be for now:* the bias defaults to 0 and is exposed as `-d3d9depthbias`.
Its original premise — wheels losing to a CPU-path road — is obsolete, because `DrawLitEnv()` now
routes the road through this same path, so wheels and road compute depth identically. What was left
was a constant pull toward the camera applied to all world geometry, biasing it against the content
still on the CPU path (blob shadows, tyre smoke, glows, decals) — precisely the things a global bias
makes fight. Verified in-game: wheels still depth-sort correctly against the road with the bias off.

### A5. Vertex fog

`agiFogMode::Vertex` reads the fog factor from per-vertex specular alpha, which `agiWorldVtx` does
not have — so world geometry renders in flat fog colour under that mode. Not the default
(`UsePixelFog` is set for D3D9 via `SpecialFlags & 0x10`) but reachable from the debug menu.
Re-express it as linear table fog over view depth, recovering the range from
`agiMeshSet::FogValue` (`= 255 / fog_end`).

### A6. `FlipX` (rear-view mirror)

`agiMeshSet::FlipX` mirrors the scene horizontally; the CPU path honours it by negating
`HalfWidth` in `InitViewport()`. The world path must negate the projection's `_11` to match, or the
mirror's world geometry and its CPU-path particles disagree about left and right inside the same
rectangle.

### A7. Device-loss handling

`agiDX9Context::ResetDevice()` calls `device_->Reset()` without releasing anything. Any
`D3DPOOL_DEFAULT` resource makes `Reset` fail — which is survivable today only because every
texture is `D3DPOOL_MANAGED` and there are no vertex buffers. Adding dynamic buffers (§1.2) makes
this mandatory: `agiRefreshable`'s existing `EndGfx()`/`BeginGfx()` pair is the natural hook.

---

## 3. Pathway B — Programmable (SM 3.0)

**Goal: per-pixel lighting and PBR materials. Free to diverge from the original, because Remix is
not watching this path.**

**Status: implemented and running.** Enabled with `-d3d9shaders`, or from `Open1560-Shaders.ini`.
Shaders live in `game/hlsl` and are compiled at runtime, so they can be edited and the game
relaunched with no C++ rebuild. B1-B5 are done; B6 is partly done (per-pixel fog with optional
height falloff, tonemapping, glow-driven point lights); B7 holds. Shadows and post-processing are
not started and are gated on the render-target work in §1.

Three things were learned the hard way and are worth carrying forward:

* **Energy conventions matter more than the BRDF.** The engine's light colours are authored as 0..1
  multipliers, not radiance. Keeping Lambert's 1/PI while feeding it those numbers made every lit
  surface 3.14x too dark and left ambient dominating the frame. Nothing about the BRDF was wrong.
* **Vertex colour carries paint, not just baked light.** Treating it purely as occlusion - defensible
  in the abstract - rendered every vehicle white, because MM1 stores car paint in the vertex stream
  and only taxis and buses have genuinely textured bodies.
* **`VFACE` is inverted on this path.** The world path submits triangles with the opposite winding to
  D3D9's convention (the same reversal Pathway A compensates for in `ToD3DCullFlipped`), so the
  textbook "negative means back-facing" test negates the normal of every *visible* pixel. That is
  what made up-facing surfaces - roads, pavements, car roofs - shade as though they pointed at the
  ground.

### B0. Glow-driven dynamic lights

Not in the original plan, and the largest visual gain so far. The city is full of things that *look*
like light sources - street lamps, traffic signals, headlights, tail and brake lights, coronas - but
which the original engine draws purely as additive billboards that emit nothing.

Those draws carry everything a real light needs: a world-space position, a colour and a size. So the
draw stream itself is the light database, with no new assets and no per-city hand placement.
Harvesting happens in two places, because glows arrive by two routes: `agiMeshSet::DrawCard` for
billboards (bangers - street lamps, traffic signals) and `agiRasterizer::MeshWorld` for glow
*meshes*, which is how all vehicle lighting is drawn. Covering only the first misses every headlight
and tail light in the game.

Points that took iteration and should not be re-litigated:

* **Lights must persist across frames, not be rebuilt each frame.** Tying a light's lifetime to
  whether its sprite happened to be drawn makes it blink out whenever the sprite is culled or an LOD
  switches. Slots are refreshed when seen, expire on a timer, and fade out rather than popping.
* **Slot matching must predict, not just compare positions.** At 200 km/h a car moves about a metre
  per frame, so a fixed distance threshold stops recognising its own headlights, allocates a new slot
  every frame, and leaves a trail of orphans behind the car. Matching against the slot's
  velocity-predicted position makes this speed-independent.
* **Selection must rank by influence, not distance.** With a small per-draw light budget and 20-50
  glows on a night street, ranking by distance to the mesh *origin* discarded the street lamp on a
  building's own wall in favour of unrelated nearer lights. Street lamps were being harvested
  correctly and then sorted out before reaching the shader. *(Obsolete — there is no per-draw
  selection any more; see B0c.)*
* **The per-draw light budget is bounded by instructions, not registers.** `ps_3_0` has no relative
  addressing for pixel-shader constants, so the light loop must unroll. Sixteen lights fit only
  because the strongest four get the full BRDF and the rest contribute diffuse only. *(Lifted — see
  B0c.)*
* **Not every glow is the same kind of light.** A street lamp exists to light a street; a tail light
  exists to be seen. They need separate intensity scales, classified by glow texture name and tint
  saturation, and exposed as settings rather than baked in.
* **Classify on RELATIVE saturation, and on the colour the shader will actually emit.** The first
  version of `ClassifyGlowIntensity` tested an absolute channel spread, `(peak - floor) > 0.40`, and
  misclassified the case documented directly above it: a street lamp measures `1.00/0.98/0.47`,
  spread `0.53`, so *every street lamp in the city* tested as a pure hue, fell through to the
  traffic-signal branch and ran at `lighttraffic` (2.0) instead of `lightlamp` (10.0). Warm white is
  by definition a wide absolute spread. It failed the other way too, because the tint handed in has
  already been multiplied by the flare's brightness — a red tail light at alpha 0.3 arrives as
  `0.30/0.00/0.00`, spread `0.30`, "unsaturated", and was classified as a *street lamp*. The two
  populations were swapped in both directions, and which way a flare went depended on how brightly
  it happened to be drawn that frame. `(peak - floor)/peak` is scale-invariant and separates the
  observed populations with room to spare. Separately, the harvester only ever sees the card's
  vertex tint, while the emitted colour is that tint modulated by `SampleGlowColor` — so the renderer
  re-runs the classification once it has resolved the real hue.
* **`DrawCard`'s position is MODEL space, and that is why street lamps lit nothing.** This one hid
  for a long time because it is invisible for half the callers. `DrawCard` projects through
  `view_params.ModelView` (= View * World), so the current world matrix applies to the position it
  is handed. `asParticles::Cull()` calls `SetWorld(IDENTITY)` before its cards, so for particles,
  smoke and vehicle glows model space *is* world space and harvesting the raw position was
  accidentally correct. `mmBangerInstance::DrawGlow()` does not — it sets the banger's own
  transform. Every street lamp therefore registered a light at its raw `mmBangerData::GlowOffset`:
  measured in the log as `(0.0, 1.8, 0.0)` for `opstlite` and `(-2.3, 6.3, 0.0)` for
  `opstlite_blue`, which are the GlowOffset values themselves, not positions in Chicago.
  Two compounding consequences: every lamp in the city piled up within a couple of metres of the
  world origin, **and** because they landed on identical coordinates, `agiAddGlowLightRGB`'s 0.9 m
  slot matcher merged them all into a *single* slot. The whole city's street lighting was one light
  at the origin. The flare still drew in the right place the whole time, because rendering uses
  ModelView and only the harvest took the number at face value — so nothing looked broken except
  that the streets were dark.
  Fix: transform by `ViewParams().World`, which is the matrix `DrawCard` is already implicitly
  using and collapses to a no-op for the identity case. Verified by the road's warmth ratio under a
  lamp going from 1.05 (neutral, indistinguishable from unlit asphalt) to 2.72.
* **Unsaturated is not the same as "street lamp".** Falling back to the lamp multiplier for anything
  that is not a pure hue handed the full lamp budget to a neutral white glow measured at `y=0.3`,
  i.e. on a vehicle. Every lamp in this game is incandescent or sodium, so a real one is *warm* -
  blue well under red. Neutral whites now take `lightgeneric` instead.
* **The two harvest routes must agree on reach.** `agiAddGlowLight` floored the reach at 24 units and
  `HarvestWorldGlow` at 14. Emitted intensity scales with the *square* of reach, so the same physical
  fixture came out roughly 3x brighter depending on whether the engine drew it as a card or as glow
  geometry. Both now go through `agiGlowLightReach()`.

### B0c. Clustered point lighting — 256 lights

**Status: implemented.** Replaces the per-draw constant-register light upload entirely.

Two limits went together. The light set lived in pixel-shader constants (`c14..c45`), and `ps_3_0`
has no relative addressing for those, so the loop had to unroll — spending the 512-slot instruction
budget linearly in the light count, which is why sixteen only fitted with twelve of them downgraded
to diffuse-only. And because a draw could only carry sixteen, the CPU had to *choose* sixteen per
draw, sorting the whole live light set against every mesh in the frame.

Lights now live in a **texture**. `tex2Dlod`'s coordinate is an ordinary float, so a loop counter can
index it: the loop body is counted once against the budget however many lights a pixel evaluates, and
every light gets the full Cook-Torrance term again. The set is built **once per frame**, in
`agiDX9WorldShader::UpdateLights()` from `BeginFrame()`, so per-draw light work is now zero — which
is the larger win in practice, because a night street submits a great many meshes.

Which lights a pixel looks at comes from a **world-space wrapping cluster grid** (`agiDX9ClusterGrid`):

| | |
|---|---|
| Grid | 32 x 8 x 32 = 8192 cells, 24 world units per cell (`-d3d9cellsize`) |
| Per cell | 16 light indices, 4 per RGBA texel, `-1` terminates |
| Pool | 256 lights, ranked by emitted energy |
| Storage | `A32B32G32R32F`, `D3DPOOL_MANAGED`, 136 KB total, two `LockRect`s per frame |
| Shader | `world.ps.hlsl`, one `rep` loop over `CELL_TEXELS`, 454/512 instruction slots |

Three choices worth not re-litigating:

* **World space, not a view-space froxel grid.** Froxels are the textbook answer, but this backend
  renders more than one view per frame — the rear-view mirror has its own view matrix and a mirrored
  projection (A6) — and a world-space grid is correct for all of them with no per-view rebuild.
* **Wrapping, not a box around the camera.** A bounded grid would have to cover the whole visible
  city, because a lamp four hundred units away still lights the street it stands on. Wrapping costs
  nothing and has no extent: two cells that alias onto one bucket are a full grid-width apart, and a
  light leaking between them is killed by its own attenuation window before it contributes.
* **Positive modulo, written out.** `fmod` and C's `%` truncate toward zero. Half the city is at
  negative X or Z, so a truncating modulo folds those onto the wrong cells and breaks clustering
  *across the origin only* — the expensive kind of half-working. The shader uses
  `c - floor(c/dim)*dim` and the builder uses the matching `WrapCell()`; they must stay in step.

Bucket overflow drops the weakest light, because insertion runs in descending energy order — an
overfull junction keeps its street lamps and loses the tail light of a car three back, which is the
right way round. `MaxLightReach` clamps a light to 128 units for both grid insertion and the shader's
window, so an `agiLighter::LIGHTS` entry with no attenuation curve (nominal range 1000) cannot land
in every cell of the grid.

The census reports `glowlights=N harvested, M pooled, K cell slots` every 120 frames.

### B0b. Lightning

The original lightning is sky-only: a thunder cue raises a flag, and the sky draw swaps in a flash
texture for a single frame and clears the flag. It never lights the city. Making it flood the scene
needs the flag latched before any drawing (the sky is drawn first and consumes it) and held with a
decay over several frames, then fed in as a broad sky-dominant irradiance burst - which is what a
strike optically is, rather than a directional light.

### B1. Shader model and fallback

Target `vs_3_0`/`ps_3_0`. Gate on
`caps.VertexShaderVersion >= D3DVS_VERSION(3,0) && caps.PixelShaderVersion >= D3DPS_VERSION(3,0)`;
anything less silently selects Pathway A. SM3.0 gives 224 float VS constants, dynamic branching,
and enough PS instruction slots for a Cook-Torrance loop over a handful of lights.

### B2. Shader storage and compilation

Follow the pattern the GL backend already established: `agigl/glrsys.cpp`'s `LoadShader()` does
`OpenFile(name, "glsl", ".vs", …)`, and the shaders live inside `game/1560.ar`
(built by `extra/build_ar.bat` → `mkar.exe`).

Mirror it exactly with an `hlsl/` folder in the same archive. Two build modes:

- **Ship:** offline `fxc` → `.cso` bytecode, packed into `1560.ar`. No `d3dx9` dependency, no
  `d3dcompiler_47.dll` redistribution question, no compile cost at startup.
- **Dev:** `PARAM_d3d9_hlsl` loads `.hlsl` source and compiles via `D3DCompile` from
  `d3dcompiler_47.dll` (`LoadLibrary`d, not link-time, so its absence is a soft failure). This
  matters here specifically because a full rebuild of this project is ~7 minutes — iterating on
  shading without recompiling C++ is the difference between an afternoon and a week.

### B3. Vertex declaration

Replace FVF with a real `IDirect3DVertexDeclaration9`, which SM3.0 wants anyway and which makes room
for the tangent frame PBR needs:

```
POSITION  0  FLOAT3   // agiWorldVtx::pos
NORMAL    0  FLOAT3   // agiWorldVtx::normal   (UnpackNormal[] expanded)
COLOR     0  D3DCOLOR // agiWorldVtx::color    (baked AO / vertex paint / DrawColor tint)
TEXCOORD  0  FLOAT2   // agiWorldVtx::tu,tv
TANGENT   0  FLOAT4   // NEW — w carries bitangent handedness
```

Tangents are not in the `.bms` format. Generate them once at mesh load
(`agiworld/meshload.cpp`) from position + UV per triangle, accumulate per adjunct, Gram-Schmidt
orthonormalise against the normal. Cache alongside the mesh; it is a one-time cost per load, not
per frame. Meshes without UVs (`TexCoords == nullptr`) skip it and use a geometric fallback.

### B4. PBR material data — where it actually comes from

MM1 ships colour maps only. There is no roughness, metalness, or normal data anywhere in the
original assets, so PBR here means *providing a place to put it*, not inventing it.

Reuse the mechanism the engine already has for texture variants. `agiDX9Pipeline::BeginGfx()` sets

```cpp
TexSearchPath = "tex16a\0tex16o\0tex16\0"_xconst;
```

and `agiTexDef::Tex.Name` is the lookup key. Add companion lookups on the same key:

| Suffix | Contents | Missing → fallback |
|---|---|---|
| `<name>` | albedo (existing texture) | — |
| `<name>_n` | tangent-space normal | flat (0,0,1) |
| `<name>_orm` | R=occlusion, G=roughness, B=metalness | from the material table |
| `<name>_e` | emissive | black |

Plus a plain-text **material table** — `pbr.csv` or similar, packed into `1560.ar` — keyed on
texture name, giving default `roughness`/`metalness`/`emissive_strength` for textures with no `_orm`
map. This is what makes the feature usable on day one: glass, chrome trim, road asphalt, and
brick can be classified by name in a few hundred lines without anyone authoring a single texture.
`agiTexProp` flags (`AlphaGlow` and friends, `agiworld/texsheet.h`) provide useful priors —
`AlphaGlow` content is emissive by definition.

Concretely: a `agiDX9Material` record hanging off `agiDX9TexDef`, resolved lazily on first bind,
exactly where `GetHandle()` already does lazy `BeginGfx()`.

### B5. Lighting

Direct lighting: Cook-Torrance — GGX/Trowbridge-Reitz distribution, Smith height-correlated
visibility, Schlick Fresnel. Lambert diffuse is sufficient; Burley adds cost for little gain at this
art fidelity.

Light sources, all already available:

- The static rig — `agiMeshLighterSun/Fill1/Fill2` + `agiMeshLighterAmbient` — as three analytic
  directionals. This is what makes buildings and terrain react to time of day per-pixel instead of
  per-vertex.
- `agiLighter::LIGHTS[0, Current)` — up to 16 point/spot lights (headlights, coronas, street
  lamps). Note this array is allowed to contain **holes**; `Current` is a high-water mark, not a
  count. Compact before uploading.
- The harvested glow registry, `agiGlowLights[]` — which in practice dwarfs the above.

*Superseded by B0c:* the original plan here was a coarse per-mesh CPU light assignment against
`agiMeshSet::Radius`. That is what shipped first and it was the bottleneck on both counts — it capped
the shader at an unrolled sixteen and it paid a sort per draw. Clustering replaced it.

Ambient/IBL: a small pre-convolved cubemap regenerated when sky colour changes
(`mmSky::Color`, `SkyColor` in `mmcity/cullcity.cpp`), sampled for diffuse irradiance, plus a
split-sum specular prefilter. This is what stops metals from reading as flat grey — a metal with no
environment term has no diffuse and no reflection, i.e. it is black.

### B6. New effects this unlocks

Ordered by ratio of visual payoff to risk:

1. **Per-pixel fog with height falloff.** Table fog is per-vertex-interpolated and flat. A shader
   can do exponential-squared distance fog *plus* a height term, which is what makes Chicago read as
   a city with air in it. Cheapest win on the list.
2. **Normal-mapped surfaces.** Brick, asphalt, cobble. Immediate and large.
3. **Proper vehicle paint.** Clearcoat lobe over a metallic base — the sphere-map hack
   (`BuildVehicleReflectionVertices`) becomes a real two-lobe BRDF with the environment cubemap.
4. **Shadow mapping for the sun.** One cascade covering the near field is enough for gameplay;
   the existing `agiMeshSet::DrawShadow`/`ShadowGeometry` blob-shadow path stays as the far-field
   fallback.
5. **Post-processing chain** — render to an `A16B16G16R16F` target, then bloom (the game is full of
   emissive glows that currently just clamp), filmic tonemap, and optional FXAA. Requires a
   render-target abstraction the backend does not have yet.
6. **Screen-space reflections** on wet roads, gated on the existing rain/snow weather state.

### B7. What Pathway B must *not* break

The HUD, text, minimap, and menus are genuinely 2D and go out pretransformed after `EndScene()`
(`agiDX9Census.ScreenCalls` minus `ScreenCallsInScene`). They must keep a trivial
pass-through shader pair, not the PBR one — and the census already distinguishes them, so the
selector is free.

---

## 4. Suggested order of work

1. §1.1 state cache — everything else is safer afterwards, and it retires an entire bug class.
2. Pathway A fixes A1/A2/A5/A6 — small, self-contained, restore parity.
3. §1.2 vertex/index buffers + §1.3 culling — the performance floor for anything ambitious.
4. A3 multi-texture — recovers the lost env/sphere maps, still Remix-safe.
5. A7 device-loss — required before §1.2 ships.
6. Pathway B, in the order of §B6.

Pathway A stays the default throughout. Pathway B is additive and never becomes load-bearing for
the Remix use case.

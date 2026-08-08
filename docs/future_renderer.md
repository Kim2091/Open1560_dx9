# Open1560 — future renderer plan (Pathway B)

Where the programmable path goes after clustered point lighting: sun shadows, better BRDFs, water and
road reflections that are not screen-space, procedural rain puddles, and ambient occlusion.

This is a design document, not a work order. Nothing here is implemented. Companion documents:
`dx9_rendering_pathways.md` (why the two pathways exist), `handoff_dx9_renderer.md` (how to work on
the backend), `light_intensities.md` (the tuning surface as it stands).

---

## 0. Two walls, and one strategic question

Read this section before planning any of the features below, because it constrains all of them.

### Wall 1 — there are no render targets

The backend has no render-target abstraction at all. It renders to the back buffer and presents.
Shadows, planar reflections, environment probes, post-processing and every screen-space AO technique
are all blocked on the same missing piece. This is the highest-leverage thing in the entire document.

It drags a prerequisite with it: `agiDX9Context::ResetDevice()` calls `Reset()` without releasing
anything, which is survivable today only because every resource is `D3DPOOL_MANAGED`. Render targets
must live in `D3DPOOL_DEFAULT`, so device-loss handling becomes mandatory rather than aspirational
(§A7 in the pathways doc). `agiRefreshable`'s existing `EndGfx()`/`BeginGfx()` pair is the hook.

### Wall 2 — the pixel shader is nearly full

`ps_3_0` allows **512 instruction slots**. `world.ps.hlsl` currently compiles to **454**. That is 58
slots of headroom, and Shader Model 3.0 is the ceiling for Direct3D 9 — there is no higher target to
move to within this API.

Rough costs for what follows, as single additions to the existing lit shader:

| feature | approx. slots |
|---|---|
| shadow lookup, 3x3 PCF | 30–40 |
| parallax-corrected cubemap probe | 20–30 |
| puddle mask + wet surface response | 25–35 |
| normal mapping with derived tangent frame | 25–30 |
| AO term (sampled from a texture) | 5–10 |

**Two of these fit. All of them do not.** Three ways out, in increasing order of cost:

1. **Reclaim budget by moving per-frame work into a post pass.** Fog, exposure, tonemap and the sRGB
   encode are all full-screen operations currently done per-pixel inside the world shader. Moving
   them into a post pass over an HDR target frees an estimated 40–50 slots *and* is a quality
   improvement in its own right. This is a happy synergy: render targets both unlock the features and
   pay for part of them.
2. **Permutations.** The `LIT` / unlit split already establishes the mechanism; generalise it to a
   small feature mask so a draw only pays for what it uses. The catch is compile time — at `/O1` the
   lit shader is ~1.5 s, and shaders are recompiled on **every** `BeginGfx()`, i.e. every load. Eight
   permutations would put ~12 s back into loading. That makes a **bytecode disk cache** (hash the
   source plus the macro set, store the compiled blob, recompile only on mismatch) a prerequisite for
   permutations rather than an optimisation. It also restores the option of `/O3` for shaders that
   actually benefit, since the cost would be paid once.
3. **A Direct3D 11 sibling — "Pathway C".** See below.

### The strategic question: should Pathway B stay on D3D9 at all?

The reason this project is on Direct3D 9 is RTX Remix, and Remix only cares about **Pathway A**, which
must stay fixed-function. Pathway B is already invisible to Remix — a vertex shader is opaque to it —
so the only thing keeping Pathway B on D3D9 is shared code below the submission layer.

A D3D11 backend would remove every constraint in this section at once: no 512-slot limit, compute
shaders (a proper clustered light *culling* pass instead of a CPU grid build, and GTAO), MRT,
structured buffers (the light texture becomes a real buffer with no packing games), hardware sRGB,
and sane render targets and resource lifetime.

The cost is real and should not be understated: a second concrete backend under `agiPipeline`, a
second texture/state path, and the loss of the current property that a single binary does both jobs.
The honest recommendation is: **do §1 and §2 on D3D9, and re-open this question before starting §4.**
If probes, puddles, AO and shadows are all wanted simultaneously, D3D9 cannot carry them and the
effort is better spent on the port than on fighting the slot budget.

---

## 1. Infrastructure (everything below depends on this)

### 1.1 Render target abstraction

Minimal shape: a colour target with optional depth-stencil, a scoped bind that restores the previous
target, and participation in `agiRefreshable` so device loss is handled.

Formats worth having: `A16B16G16R16F` for the HDR scene, `R32F` or `D24X8`+`INTZ` for shadow depth,
`A8R8G8B8` for probes and reflections.

### 1.2 HDR scene target and a post pass

Once (1.1) exists, render the world into an HDR target and add a full-screen pass doing fog, exposure,
tonemap, sRGB encode, and later bloom and antialiasing. As noted above this both improves quality
(bloom on emissive content that currently clamps; fog computed once rather than per world pixel) and
buys back the shader budget the rest of this document needs.

### 1.3 Vertex/index buffers and frustum culling

Not glamorous, but shadows and planar reflections each add a **full extra submission pass** over the
world geometry. Today every draw is `DrawIndexedPrimitiveUP` with a per-frame CPU vertex build and no
CPU-side culling (§1.2/§1.3 in the pathways doc). Doubling or tripling that submission cost before
fixing it will make the frame CPU-bound and will misattribute the cost to the new features.

**Treat static vertex buffers and frustum culling as prerequisites for §2 and §5, not as separate
optimisation work.**

---

## 2. Direct sun lighting and shadows

The largest remaining visual gain. The sun already drives the scene per-pixel via
`agiMeshLighterSun`; what is missing is occlusion.

### 2.1 Single cascade, near field

One orthographic cascade covering roughly 80–120 world units around the player is enough for
gameplay. The existing `agiMeshSet::DrawShadow` blob-shadow path stays as the far-field fallback,
which also means a graceful degradation story if the cascade is disabled.

Multiple cascades are a later refinement. A second cascade is worth far less here than getting the
first one stable.

### 2.2 The things that actually go wrong

* **Shimmer.** Snap the light-space ortho origin to whole shadow-map texels each frame. Without it the
  map re-rasterises differently every frame and every shadow edge crawls. This is the single most
  common thing to get wrong and it is very visible in a driving game.
* **Bias.** Slope-scaled depth bias plus normal-offset. Note a project-specific hazard: mesh normals
  are quantised to a 198-entry table roughly 26° apart, so **normal-offset bias is unreliable on
  meshes that have not been through `smoothnormals`**. Either require smoothing for the shadow path
  or lean on slope-scaled bias alone and accept more peter-panning.
* **What casts.** Cars, bangers and building shells should cast. Alpha-keyed foliage needs alpha
  testing in the depth pass or it casts solid rectangles. `agiTexProp::AlphaGlow` content must not
  cast at all — a light flare is not an occluder.
* **Filtering.** 3x3 PCF is the floor for acceptable edges. Hardware PCF availability varies by
  vendor on D3D9; a manual PCF over `R32F` is the portable choice and costs slots (see Wall 2).

### 2.3 Sun as an area light

Cheap and worth doing at the same time: the sun is a ~0.5° disc, not a point. A representative-point
or simple roughness-widening approximation softens the specular highlight and stops sunlit car paint
reading as a pinpoint star. A few slots for a visible gain.

### 2.4 Shadows from the glow lights

Wanted at night, but 256 shadow-casting lights is not happening. Realistic options, cheapest first:

1. **None.** Rely on the fact that the lights are numerous and dim; missing occlusion reads as a soft
   ambient wash rather than as an error.
2. **One or two cube shadow maps** for the highest-energy lights near the player — usually the
   player's own headlights, which is where a missing shadow is most noticeable.
3. **Screen-space contact shadows** — a short depth-buffer ray march per light. Effective and cheap
   for the near field, but it is a screen-space technique with the usual off-screen failure, and the
   brief for reflections explicitly avoids screen space. Judge it on its own merits.

Recommendation: ship (1), evaluate (2) for headlights only.

---

## 3. Advanced lighting models

Current: Cook-Torrance with GGX distribution, Smith height-correlated visibility, Schlick Fresnel,
Lambert diffuse. That is a reasonable base. Ordered by payoff:

### 3.1 Sphere-area point lights — the highest-value change

The attenuation denominator is currently clamped at one world unit as a stand-in for a finite source
size. `light_intensities.md` §0 shows what that costs: everything within ~7 units of a lamp is
saturated, gain values run to the hundreds, and ACES is the only thing preventing white blobs.

Replacing it with Karis' representative-point sphere light — treat the lamp as a sphere of radius
*r*, move the shading point's light vector to the closest point on that sphere, and normalise energy
by the sphere's solid angle — makes the near field behave, makes the specular highlight the right
*shape* (an elongated smear on wet asphalt rather than a dot), and would let the intensity numbers
become physically meaningful rather than empirically dialled.

**This should probably precede the intensity tuning pass**, because it changes what the numbers mean.

### 3.2 Multi-scatter GGX energy compensation

Single-scatter GGX loses energy at high roughness, which is most of this city (asphalt at 0.88,
damaged surfaces at 0.95). A one-line compensation term using the same split-sum fit already present
in `EnvBRDFApprox` restores it. Very cheap, small but broad improvement.

### 3.3 Clearcoat for vehicle paint

A second, low-roughness specular lobe over the existing dielectric base. Automotive paint is pigment
under lacquer and the two-lobe response is what makes it read as a car rather than as plastic. Pairs
with §4 — a clearcoat lobe with nothing to reflect is invisible, so this wants probes first.

### 3.4 Normal mapping

Plumbing decision already made: derive the tangent frame from screen-space derivatives rather than
changing the vertex format or the mesh loader, both of which are constrained by the frozen ARTS
layouts. The blocker is not technical, it is that **there are no normal maps in the game** — this is
worth nothing until someone authors them, so it should be sequenced after everything that improves
the existing assets.

---

## 4. Reflections without screen space — environment probes

The requirement is road and vehicle reflections that do not depend on what is on screen. The answer is
**parallax-corrected cubemap probes**.

### 4.1 Why probes fit this game specifically

MM1 already partitions the city spatially — `mmCullCity` works in rooms/cells, and the `.bng` banger
placements carry a `Room` index. That partition is a ready-made probe layout: **one probe per room**,
with the room's bounds as the parallax proxy box. No new authoring, no hand placement, and it reuses a
structure the engine already maintains and culls against. (Confirm the exact room accessor before
relying on it; the culler is largely closed asm.)

### 4.2 Mechanics

* Bake a low-resolution cubemap (64² or 128² per face is plenty for rough surfaces) per room, either
  offline into the archive or at city load.
* Prefilter into roughness mips — the split-sum convention the shader's `EnvBRDFApprox` already
  assumes, so the BRDF side needs no change.
* At shade time, intersect the reflection ray with the room's proxy box and sample the cubemap along
  the corrected direction. Parallax correction is what stops a reflection sliding as the camera moves.
* Blend between the two nearest probes to avoid popping at room boundaries.

### 4.3 What this unlocks beyond wet roads

Metals stop being black. Right now **nothing in the material table is metallic** — deliberately, per
`agiDX9ResolveMaterial`, because a metal with no environment has no diffuse and no reflection.
Chrome trim, glass, and the clearcoat lobe in §3.3 all become possible only once probes exist. This is
the widest-reaching item in the document after shadows.

### 4.4 Roads specifically

Dry asphalt at roughness 0.88 barely reflects; the interesting case is **wet** asphalt (§6), where
roughness drops and the probe suddenly matters a great deal. A heavily blurred probe mip is enough —
wet asphalt is not a mirror, and trying to make it one is what makes screen-space approaches look
wrong.

The current grazing-angle damping hack (`indirect *= (1 - roughness)`) exists precisely because the
hemisphere stand-in has no real environment. Probes let it be deleted in favour of something physical.

---

## 5. Water reflections

Lake Michigan and the river are large, flat, and horizontal — the ideal case for **planar
reflection**, which is exact, has no off-screen failure, and is much simpler than it looks.

### 5.1 Mechanics

* Find the water plane (a constant Y per body; likely derivable from the city data — verify rather
  than hardcode).
* Mirror the view matrix about the plane, render the scene into a half-resolution target.
* Sample it with projected screen coordinates, perturbed by the water normal for ripples.
* Blend with Fresnel: near-grazing angles reflect strongly, overhead views show water colour.

### 5.2 The one hard part

Geometry below the water plane must not appear in the reflection. Standard fix is **oblique near-plane
clipping** — skew the projection matrix so the near plane coincides with the water plane (Lengyel's
method). D3D9 user clip planes are an alternative but interact awkwardly with vertex shaders across
drivers; the oblique projection is the portable choice.

### 5.3 Ripples

Two or three scrolling normal-map layers at different scales and speeds, summed. Cheap and
convincing. This shares the noise infrastructure with §6.

### 5.4 Cost control

The reflection pass is a second full scene submission — see §1.3. Mitigations: half resolution, a
reduced draw distance, skipping small props, and only running it when water is actually in frame
(which the room/cell partition can answer cheaply).

---

## 6. Rain puddles from noise

The cheapest large win in this document, and the only one that needs **no render target at all** —
only shader budget. If §1.2 frees the slots, this can land before shadows.

### 6.1 Wetness state

Drive from the engine's own weather (`MMSTATE.Weather`) rather than inventing a system. Important:
wetness must **accumulate and dry over time**, not switch instantly with the weather flag — a street
that becomes wet the same frame the rain starts looks wrong, and the drying tail is most of the
visual interest. A single global scalar updated per frame is sufficient; per-surface wetness is not
worth the complexity here.

### 6.2 The puddle mask

Sample a tiling noise texture in **world XZ** (not screen space, or puddles swim as the camera moves)
and threshold against wetness so puddles grow as it rains and shrink as it dries.

Prefer a small tiling texture with several octaves packed into its channels over analytic hash noise
computed in-shader — the ALU cost of good procedural noise is exactly what the slot budget cannot
afford (Wall 2), and a 64² four-channel texture is free by comparison.

Restrict puddles to surfaces flagged `agiTexProp::RoadFloorCeiling` so they do not appear on walls or
car roofs. MM1 has no fine road height detail, so puddles cannot pool in genuine low spots; the noise
mask is standing in for that and should be tuned to look plausible rather than physical.

### 6.3 Surface response

Inside the mask: roughness drops sharply (toward 0.05–0.15), the normal flattens toward straight up,
and the diffuse albedo darkens slightly — wet asphalt is darker as well as shinier, and skipping the
darkening is what makes naive wetness read as "shiny plastic road".

### 6.4 Ripple rings

While rain is actively falling, perturb the puddle normal with animated concentric rings. A small
animated ripple texture with randomised per-puddle phase is enough; individually simulated droplets
are not worth it at this fidelity.

### 6.5 Why this pairs with §4

A puddle at roughness 0.1 is a mirror with nothing to reflect unless probes exist. **Puddles without
§4 will look like grey shiny patches.** Either sequence probes first, or accept that the first
iteration will be underwhelming and plan to revisit.

---

## 7. Ambient occlusion

Three options with genuinely different tradeoffs. They are not mutually exclusive and the best answer
is probably two of them layered.

### 7.1 Baked per-vertex AO (static geometry)

Compute AO offline per mesh and store it. Class layouts are frozen, so it **cannot** be a new field on
`agiMeshSet` — it goes in a side table keyed on the mesh, which has precedent in the codebase.

Pros: free at runtime, no render target, no screen-space artifacts, correct for large-scale occlusion
(under bridges, inside arcades, alley corners) that screen-space methods handle badly.
Cons: static only, needs an offline bake tool, and interacts with the existing use of vertex colour —
which in MM1 carries **paint as well as baked light**, so AO must be a separate channel and not folded
into the existing colour.

### 7.2 Analytic capsule/box AO (dynamic objects)

Approximate each vehicle as a capsule or box and darken the ground beneath it analytically. Cheap, no
render target, no artifacts, and it is a strict improvement on the existing blob-shadow texture for
grounding cars. Handles exactly the case (1) cannot.

### 7.3 GTAO / HBAO (screen space)

The general solution, and the one that needs a depth target and a meaningful slot budget. Highest
quality for fine contact detail, but it is screen-space: occluders outside the frame do not occlude,
and there is a temporal-stability cost. Given the explicit preference for non-screen-space techniques
elsewhere in this plan, treat it as optional rather than as the target.

**Recommendation: (7.1) + (7.2) as the base.** Together they cover static and dynamic occlusion with
no render target and no screen-space failure modes, which suits both the constraints and the stated
preference. Add (7.3) later only if fine contact detail is missed.

---

## 8. Suggested order

Dependency-ordered. The bracketed items are the walls from §0.

1. **[Wall 1]** Render targets + device-loss handling (§1.1).
2. HDR target and post pass (§1.2) — quality win, and **buys back the slot budget** for everything below.
3. Static vertex/index buffers + frustum culling (§1.3) — before anything adds a second scene pass.
4. **Sun shadows**, single cascade, stabilised (§2.1–2.2). Largest single visual gain.
5. Sphere-area lights (§3.1) + multi-scatter GGX (§3.2) — cheap, and §3.1 should land **before** the
   light-intensity tuning pass because it changes what the numbers mean.
6. **[Decision point]** Re-open the D3D11 question (§0) before committing to 7–9. If all of probes,
   puddles and AO are wanted, D3D9 cannot carry them alongside shadows.
7. Environment probes (§4) — unlocks metals, clearcoat, and makes §6 worth doing.
8. Rain puddles (§6) and water planar reflection (§5).
9. AO: baked vertex + analytic capsule (§7.1, §7.2).
10. Clearcoat vehicle paint (§3.3); normal mapping (§3.4) only once maps are authored.

Two standing constraints throughout: **Pathway A stays the default and stays fixed-function**, and
every Pathway B failure stays soft — no shader compiler, insufficient hardware, or a shader that fails
to compile must all fall back to Pathway A with a log line, exactly as they do now.

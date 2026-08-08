//  Open1560 - Pathway B world pixel shader (ps_3_0)
//
//  Cook-Torrance PBR replacement for the fixed-function per-vertex lighting on the world-space
//  path. See docs/dx9_rendering_pathways.md and world.vs.hlsl.
//
//  Two permutations, selected by the LIT macro from dx9shader.cpp:
//    LIT      - full PBR. Meshes that carry per-vertex normals.
//    (unlit)  - meshes loaded without MESH_SET_NORMAL, whose vertex colours are already final.
//               Most static scenery and every low-detail LOD. Nothing to light.
//
//  Colour space: MM1 textures are authored in sRGB. We decode to linear, light in linear, tonemap,
//  then encode back. The FF path did all of this in gamma space - that, more than any BRDF detail,
//  is why this looks different: gamma-space lighting darkens midtones and blows out highlights.
//
//  Vertex colour is NOT a light value here. On the FF path it *was* the lighting. Under PBR we
//  compute the lighting ourselves, so re-multiplying it would double-count. It is treated as baked
//  ambient occlusion plus per-instance tint and applied to the indirect term only.

float4 g_Material    : register(c0);  // x=roughness y=metalness z=emissive w=vertex-colour AO amount
float4 g_FogParams   : register(c1);  // x=start y=end z=height falloff w=enable
float4 g_FogColor    : register(c2);
float4 g_CameraPos   : register(c3);  // xyz=world position, w=exposure
float4 g_Ambient     : register(c4);
float4 g_SkyColor    : register(c5);  // hemisphere "up" irradiance
float4 g_GroundColor : register(c6);  // hemisphere "down" irradiance
float4 g_LightDir[3] : register(c7);  // c7..c9    static rig: sun, fill1, fill2 (direction TO light)
float4 g_LightCol[3] : register(c10); // c10..c12

// --- Clustered point lights ---------------------------------------------------------------------
//
// Point lights used to live in constant registers - c14..c45, sixteen of them - and that is what
// capped the light count. ps_3_0 has no relative addressing for pixel-shader constants (only vertex
// shaders get a0/aL), so a constant-register light loop has to unroll, and an unrolled loop spends
// the 512 instruction slots linearly in the light count. Sixteen only fitted because the last twelve
// were downgraded to diffuse-only. It also meant the CPU had to pick those sixteen per DRAW, sorting
// the whole live light set against every mesh in the frame.
//
// Lights now live in a TEXTURE. A tex2Dlod coordinate is an ordinary float, so a loop counter can
// index it, which means one loop body for any number of lights - and the set can be prepared once
// per frame instead of once per draw. Every light gets the full BRDF again as a result, because the
// body is counted once rather than sixteen times.
//
// Which lights a pixel looks at comes from a world-space cluster grid: g_CellTex holds, per grid
// cell, the indices of the lights that reach it. The grid WRAPS rather than being a box around the
// camera, so it covers the whole city - a lamp four hundred units away still lights the street it
// stands on - and two cells that alias onto one bucket are a full grid-width apart, which their own
// attenuation window rejects for free. It is world-space rather than the textbook view-space froxel
// grid because this backend renders more than one view per frame (the rear-view mirror has its own
// view matrix and a mirrored projection) and a world-space grid is correct for all of them with no
// rebuild. See agiDX9ClusterGrid in agidx9/dx9shader.h.

// Light indices per cell, and how many texels that is at 4 indices per texel. Must match
// agiDX9ClusterGrid::LightsPerCell.
#define CELL_LIGHTS 16

// One index per texel - the bucket texture is R32F. ps_3_0 has no relative addressing for
// temporaries, so the components of a fetched float4 cannot be looped over and four indices to a
// texel meant the light body was emitted four times. At a measured 59 slots per inlining that was
// 454 of 512 used. One per texel is one body. See agiDX9ClusterGrid::TexelsPerCell.
#define CELL_TEXELS CELL_LIGHTS

float4 g_GridDim  : register(c13); // xyz = cells per axis, w = 1 / cell size
float4 g_GridTex  : register(c14); // x = cells per texture row, y = cell tex width, z = height,
                                   // w = specular from point lights on/off
float4 g_LightTexInfo : register(c15); // x = 1 / max lights (light texture width)
float4 g_EnvInfo : register(c16); // x = highest mip index, y = reflection strength, z = draw reflectivity, w = probe
// View rotation, transposed into columns, so a world-space vector reaches view space in three dots.
// c17.w is 1 when this draw has an authored sphere map bound on s4.
float4 g_ViewCol[3] : register(c17); // c17..c19

sampler2D g_Albedo   : register(s0);
sampler2D g_LightTex : register(s1); // row 0: xyz = world pos, w = 1/reach^2. row 1: rgb = colour
sampler2D g_CellTex  : register(s2); // x = one light index, -1 = empty (see CELL_TEXELS)
samplerCUBE g_EnvCube : register(s3); // analytic sky/ground/sun probe, mip = roughness (dx9probe.h)
sampler2D g_SphereMap : register(s4); // the game's own authored vehicle reflection texture

static const float PI = 3.14159265f;

// Lambert's 1/PI, deliberately folded out.
//
// A textbook Lambert BRDF is albedo/PI, and the light term it multiplies is expected to be
// *radiance*. The engine's light colours are not radiance - agiMeshLighterSunColor and friends are
// plain multipliers, used by the CPU rig (agiworld/meshlight.cpp, mmxTriple) as
//     intensity = sun.col*NdotL + fill1.col*NdotL + fill2.col*NdotL + ambient
//     out       = vertex_colour * intensity
// so a white surface facing a full-strength light returns the light colour, not the light colour
// over PI. Keeping the 1/PI while feeding it those same numbers made every directly-lit surface
// 3.14x too dark, which left the (much weaker) ambient term dominating the image - the flat,
// washed-out look, and a *yellow* car body rendering near-black.
//
// The physically-tidy alternative is to scale every light colour by PI on upload; that is the
// identical computation, and it would misrepresent the engine's authored values as radiance. This
// way the direct diffuse response matches the original rig term for term, which is what Pathway B
// should be starting from before it improves on anything.
static const float INV_PI_COMPENSATED = 1.0f;

struct VSOut
{
    float4 Position : POSITION;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
    float3 Normal   : TEXCOORD2;
    float4 Color    : COLOR0;
};

// Cheap sRGB transfer approximations. The exact curve needs a pow() plus a conditional linear
// segment near black; at 1999 texture fidelity the squared/sqrt approximation (gamma 2.0 rather
// than 2.2) is visually indistinguishable and several instructions cheaper - which matters when it
// runs on every pixel of the city.
float3 SrgbToLinear(float3 c)
{
    return c * c;
}

float3 LinearToSrgb(float3 c)
{
    return sqrt(max(c, 0.0f));
}

// --- BRDF -------------------------------------------------------------------------------------
//
// Cook-Torrance specular: D * V * F
//   D - GGX / Trowbridge-Reitz. Its wide tail is what makes rough metal read as metal and not as
//       plastic; Blinn-Phong (which is what the FF pipeline's specular power was) cannot.
//   V - Smith height-correlated visibility, pre-divided by the 4*NdotL*NdotV denominator so it is a
//       visibility term rather than a raw geometry term, saving a divide.
//   F - Schlick.
// Diffuse is Lambert. Burley/Disney diffuse costs more and buys nothing here: its retroreflective
// term is driven by a roughness map, and we have none.

float D_GGX(float NdotH, float a)
{
    float a2 = a * a;
    float d = (NdotH * a2 - NdotH) * NdotH + 1.0f;
    return a2 / max(PI * d * d, 1e-7f);
}

float V_SmithHeightCorrelated(float NdotV, float NdotL, float a)
{
    // Hammon's approximation - one lerp instead of two square roots, error far below visible.
    float a2 = a * a;
    float lv = NdotL * (NdotV * (1.0f - a2) + a2);
    float ll = NdotV * (NdotL * (1.0f - a2) + a2);
    return 0.5f / max(lv + ll, 1e-7f);
}

float3 F_Schlick(float3 f0, float VdotH)
{
    float f = pow(1.0f - VdotH, 5.0f);
    return f0 + (1.0f - f0) * f;
}

// Karis' analytic fit to the split-sum environment BRDF. Stands in for the precomputed 2D LUT a
// full IBL pipeline would sample - there are no render targets on this path yet, and at this
// quality level the fit is more than adequate.
float3 EnvBRDFApprox(float3 f0, float roughness, float NdotV)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * NdotV)) * r.x + r.y;
    float2 ab = float2(-1.04f, 1.04f) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// One clustered point light, fetched by index out of g_LightTex.
//
// Returns black for an empty slot (index < 0) and for anything outside its own reach, so the caller
// can accumulate unconditionally rather than branching per light - a per-light dynamic branch in
// ps_3_0 costs more in predication than the arithmetic it skips.
float3 EvalPointLight(float index, float3 world_pos, float3 N, float3 V, float3 diffuse_color, float3 f0, float a,
    float NdotV)
{
    // Empty slots are written as -1 by the grid builder.
    if (index < 0.0f)
        return 0.0f;

    float u = (index + 0.5f) * g_LightTexInfo.x;

    float4 light_pos = tex2Dlod(g_LightTex, float4(u, 0.25f, 0.0f, 0.0f));
    float4 light_col = tex2Dlod(g_LightTex, float4(u, 0.75f, 0.0f, 0.0f));

    float3 to_light = light_pos.xyz - world_pos;
    float dist2 = dot(to_light, to_light);

    // Inverse-square falloff with a smooth windowed cutoff, so a light fades to nothing at its reach
    // instead of popping when it leaves the cell or the pool. light_pos.w is 1/reach^2.
    //
    // The window also does the real culling: a grid cell is a box, so a light in the cell's list can
    // still be out of range of this particular pixel, and two cells that alias onto the same bucket
    // are a whole grid-width apart. Both cases land here and return black.
    float window = saturate(1.0f - dist2 * light_pos.w);

    if (window <= 0.0f)
        return 0.0f;

    float3 L = to_light * rsqrt(max(dist2, 1e-6f));
    float NdotL = saturate(dot(N, L));

    // The denominator is clamped to 1 world unit, not to an epsilon. Inverse-square diverges at the
    // source, and these lights sit *on* geometry - a tail light is centimetres from the bodywork, a
    // headlight from the road it points at - so an epsilon clamp let attenuation reach ~10^4 and blew
    // whole regions to flat white. Clamping to a finite source radius treats the lamp as a small
    // sphere rather than a point, which is what it physically is.
    float atten = window * window / max(dist2, 1.0f);

    float3 contribution = diffuse_color * INV_PI_COMPENSATED;

    if (g_GridTex.w > 0.5f)
    {
        float3 H = normalize(L + V);
        contribution += D_GGX(saturate(dot(N, H)), a) * V_SmithHeightCorrelated(NdotV, NdotL, a) *
            F_Schlick(f0, saturate(dot(V, H)));
    }

    return contribution * light_col.rgb * (NdotL * atten);
}

// Narkowicz's ACES filmic curve. Applied here rather than in a post pass because there is no HDR
// render target on this path. It still earns its place: the lighting sum genuinely exceeds 1.0
// (three directionals plus up to four point lights), and without it every sunlit facade and every
// headlight pool hard-clips to flat white.
float3 TonemapACES(float3 x)
{
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Fog is applied in LINEAR space, before the tonemap, so distant geometry converges on the fog
// colour the same way real aerial perspective does. Applying it after the tonemap would let the
// curve act on the fogged result and desaturate the horizon.
float3 ApplyFog(float3 color, float3 world_pos, float view_dist)
{
    if (g_FogParams.w < 0.5f)
        return color;

    // Distance term: linear, matching the engine's own ramp (agiCurState's fog start/end) so this
    // path still agrees with the CPU path on content that has not moved over yet.
    float f = saturate((view_dist - g_FogParams.x) / max(g_FogParams.y - g_FogParams.x, 1e-4f));

    // Height term: exponential falloff with altitude. This is the part fixed-function fog cannot
    // express at all - table fog is purely radial, so a street and a tower roof at equal distance
    // get equal fog. Adding altitude is what makes the skyline recede and the streets read as
    // having air in them.
    f *= exp(-max(world_pos.y, 0.0f) * g_FogParams.z);

    return lerp(color, SrgbToLinear(g_FogColor.rgb), saturate(f));
}

// VFACE sign is INVERTED on this path relative to the usual D3D9 convention, and getting that
// backwards inverted every normal in the scene.
//
// The textbook reading is "VFACE < 0 means back-facing", so the obvious code is
//     N = normalize(i.Normal) * (face < 0 ? -1 : 1)
// which flips normals on back faces. On this path that expression negated the normal of every
// *visible* pixel, because D3D9 decides front-vs-back from triangle winding - and the world-space
// path submits its triangles with the opposite winding to D3D9's convention. That is not a new
// discovery: Pathway A already had to compensate for exactly this with ToD3DCullFlipped()
// (dx9rsys.cpp), because MeshWorld()'s view-matrix z-flip is a reflection and reverses triangle
// orientation. VFACE keys off the same winding, so it reports our front faces as "back".
//
// Measured, not guessed: rendering sign(dot(N, sun)) showed roads and car roofs RED (facing away)
// while the sun was independently sampled in gameplay as (-0.043, +0.929, 0.357) - i.e. pointing
// firmly up, and MM1 is Y-up. An up-facing road under an up-pointing sun cannot have a negative
// dot product unless the normal has been negated. That was the "lighting looks inverted, dark on
// top for no reason" symptom exactly: every up-facing surface in the city - roads, pavements, car
// roofs and bonnets - was being lit as though it pointed at the ground.
//
// So the test is inverted here: our front faces are the ones with face < 0. Two-sided draws
// (agiCullMode::None) still get their inward faces flipped correctly, which is why this keeps using
// VFACE rather than dropping it - a sign(dot(N,V)) test would instead produce a dark rim around the
// silhouette of legitimately front-facing curved geometry.
float4 main(VSOut i, float face : VFACE) : COLOR0
{
    float4 tex = tex2D(g_Albedo, i.UV);

    // Vertex colour modulates ALBEDO, not just the indirect term.
    //
    // This shader originally treated vertex colour purely as baked occlusion, on the reasoning that
    // it was the fixed-function path's *lighting* and re-applying it under PBR would double-count.
    // That is only half true for this engine. In MM1 the vertex stream also carries per-instance
    // PAINT: a car body's colour lives there, not in its texture - which is why vehicles came out
    // white while taxis and buses, which have genuinely textured bodies, looked correct.
    //
    // Paint and baked light cannot be separated after the fact, so the choice is which error to
    // make. Multiplying is the smaller one by a wide margin: it reproduces what the fixed-function
    // path does, it restores vehicle colours, and the residual double-count of baked light is a
    // gentle darkening in creases - where a shadow belongs anyway. Discarding it loses information
    // that exists nowhere else.
    float3 albedo = SrgbToLinear(tex.rgb) * SrgbToLinear(i.Color.rgb);
    float alpha = tex.a * i.Color.a;

    float3 to_eye = g_CameraPos.xyz - i.WorldPos;
    float view_dist = length(to_eye);
    float3 V = to_eye / max(view_dist, 1e-5f);

#ifdef LIT
    float3 N = normalize(i.Normal) * (face < 0.0f ? 1.0f : -1.0f);

    float NdotV = saturate(dot(N, V)) + 1e-5f;

    float roughness = saturate(g_Material.x);
    float metalness = saturate(g_Material.y);

    // Perceptual roughness -> GGX alpha. Squaring is the standard remap: it makes a linear 0..1
    // material slider behave evenly to the eye instead of collapsing the interesting range into the
    // bottom fifth.
    float a = max(roughness * roughness, 1e-3f);

    float3 diffuse_color = albedo * (1.0f - metalness);
    float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metalness);

    float3 direct = 0.0f;

    // --- Static rig: sun + two fills -----------------------------------------------------------
    // agiMeshLighterSun/Fill1/Fill2 - the same three directions and colours the CPU rig uses, so
    // time of day and weather still drive the scene exactly as the engine intends. Only the
    // response curve changes.
    [unroll]
    for (int s = 0; s < 3; ++s)
    {
        float3 L = g_LightDir[s].xyz;
        float NdotL = saturate(dot(N, L));

        float3 H = normalize(L + V);
        float NdotH = saturate(dot(N, H));
        float VdotH = saturate(dot(V, H));

        float3 spec = D_GGX(NdotH, a) * V_SmithHeightCorrelated(NdotV, NdotL, a) * F_Schlick(f0, VdotH);
        direct += (diffuse_color * INV_PI_COMPENSATED + spec) * g_LightCol[s].rgb * NdotL;
    }

    // --- Clustered point lights ------------------------------------------------------------------
    // Street lamps, traffic signals, lit signage, coronas, head/tail/brake lights - the game's own
    // glow sprites, harvested into real light sources (agiworld/glowlight.h) plus whatever the engine
    // itself has in agiLighter::LIGHTS.
    //
    // Which cell this pixel is in. The modulo is written as `c - floor(c/dim)*dim` rather than fmod
    // because fmod truncates toward zero and half the city is at negative X or Z; truncation would
    // fold those onto the wrong cells and break clustering across the origin only, which is exactly
    // the kind of half-working that is expensive to notice. The CPU builder uses the matching
    // WrapCell().
    float3 cell = floor(i.WorldPos * g_GridDim.w);
    cell -= floor(cell / g_GridDim.xyz) * g_GridDim.xyz;

    float cell_index = ((cell.z * g_GridDim.y) + cell.y) * g_GridDim.x + cell.x;

    // Buckets are laid out CellsPerRow to a texture row, TexelsPerCell texels each.
    float cell_row = floor(cell_index / g_GridTex.x);
    float cell_col = cell_index - (cell_row * g_GridTex.x);

    float cell_v = (cell_row + 0.5f) / g_GridTex.z;
    float cell_u = ((cell_col * (float) CELL_TEXELS) + 0.5f) / g_GridTex.y;
    float cell_du = 1.0f / g_GridTex.y;

    // A real loop, not an unrolled one - that is the whole point of moving the lights into a
    // texture. The body is counted once against the instruction budget however many lights the cell
    // holds, and the [loop] break means a cell with two lights in it costs two lights' work rather
    // than the worst case.
    //
    // One EvalPointLight call, deliberately. Four calls here - one per component of an RGBA bucket
    // texel - cost four inlined copies of the entire Cook-Torrance body, because ps_3_0 cannot index
    // a temporary register dynamically and so cannot loop over slot.x/y/z/w. That was 454 of 512
    // slots. With one index per texel the trip count carries the remaining lights for free.
    [loop]
    for (int t = 0; t < CELL_TEXELS; ++t)
    {
        float index = tex2Dlod(g_CellTex, float4(cell_u + ((float) t * cell_du), cell_v, 0.0f, 0.0f)).x;

        // The builder fills slots in order and writes -1 to the rest, so the first empty slot ends
        // the cell.
        if (index < 0.0f)
            break;

        direct += EvalPointLight(index, i.WorldPos, N, V, diffuse_color, f0, a, NdotV);
    }

    // --- Indirect ------------------------------------------------------------------------------
    // Hemisphere approximation rather than a prefiltered cubemap: sky irradiance from above,
    // bounced ground irradiance from below. Cheap, needs no render target, and it is what stops
    // metals reading as black - a metal has no diffuse lobe, so with no environment term at all it
    // has nothing to reflect.
    // g_SkyColor / g_GroundColor are the engine's own ambient split into up and down lobes by
    // dx9shader.cpp - NOT the fog or sky colour. See the note there: the preset table gives Rain a
    // SkyColor of 0x000000 and leaves it stale entirely in clear weather, and because this term is
    // indirect light it applies at all distances, so driving it from fog washed the whole frame.
    // g_Ambient is deliberately not added on top; that was double-counting the same energy.
    float hemi = N.y * 0.5f + 0.5f;
    float3 irradiance = lerp(g_GroundColor.rgb, g_SkyColor.rgb, hemi) + g_Ambient.rgb;

    // No separate vertex-colour tint here any more - it is already in `albedo` above, so applying
    // it again would square it and crush every painted surface.
    float3 indirect = diffuse_color * irradiance;

    // Environment specular.
    //
    // With a real prefiltered probe this is finally the textbook split-sum term: a reflection vector
    // into a cubemap whose mip level stands for roughness, multiplied by the EnvBRDF fit. That is
    // what makes car paint read as paint - it has something specific to reflect (sky above, road
    // below, a hot sun lobe) instead of one flat irradiance value from every direction.
    //
    // It also retires a workaround. This used to be `irradiance * EnvBRDF * (1 - roughness)`, and
    // the damping term was a judgement call to kill a broad wet-looking sheen that swept down the
    // carriageway: a road is enormous, flat and viewed almost edge-on, so EnvBRDFApprox's grazing
    // Fresnel rise against a single bright irradiance value lit the whole thing. A prefiltered probe
    // fixes that at the source rather than by subtraction, because a rough road samples the bottom
    // of the mip chain and sees the dim ground colour, not the bright sky. The note in
    // dx9_rendering_pathways.md §B6 called this out as the physically tidier fix; this is it.
    //
    // g_EnvInfo.w is 0 when no probe could be created, in which case the old flat term is kept -
    // worse looking, still correct.
    float3 R = reflect(-V, N);

    if (g_ViewCol[0].w > 0.5f)
    {
        // --- Vehicle bodywork: the game's own authored reflection map, as a clearcoat ------------
        //
        // Cars take the sphere map ALONE, not blended with the synthesised probe. MM1 drew that
        // texture specifically for bodywork and it is what the vehicles were designed against; the
        // probe exists for the surfaces the game never authored a reflection for.
        //
        // The mapping is the one BuildVehicleReflectionVertices() established for the fixed-function
        // pass, reproduced per pixel rather than per vertex: reflection vector into view space, then
        // m = 2*sqrt(Rx^2 + Ry^2 + (Rz+1)^2) and read (Rx/m + 0.5, 0.5 - Ry/m).
        float3 Rv;
        Rv.x = dot(R, g_ViewCol[0].xyz);
        Rv.y = dot(R, g_ViewCol[1].xyz);
        Rv.z = dot(R, g_ViewCol[2].xyz);

        float m = 2.0f * sqrt(dot(Rv.xy, Rv.xy) + ((Rv.z + 1.0f) * (Rv.z + 1.0f)));

        float2 sph = (m > 1e-5f) ? float2((Rv.x / m) + 0.5f, 0.5f - (Rv.y / m)) : float2(0.5f, 0.5f);

        // Fresnel-weighted, and this is the important part.
        //
        // Applying the reflection at a near-uniform strength across the panel is what made cars look
        // shrink-wrapped: real lacquer reflects hard at grazing angles and barely at all head-on, so
        // a flat application reads as a sticker rather than as a surface. Schlick with a dielectric
        // clearcoat F0 of 0.04 is the physical answer and costs three instructions - the reflection
        // now gathers at the edges of a panel and along its curvature, which is where the eye
        // expects it.
        //
        // The previous 2.5x flat multiplier is gone with it. Full strength at grazing is already
        // correct; d3d9reflect scales the whole thing if a stronger look is wanted.
        float fresnel = pow(1.0f - NdotV, 5.0f);
        float coat = 0.04f + (0.96f * fresnel);

        indirect += tex2D(g_SphereMap, saturate(sph)).rgb * coat * g_EnvInfo.y;
    }
    else if (g_EnvInfo.w > 0.5f)
    {
        // --- Everything else: the prefiltered probe ----------------------------------------------
        //
        // Rough surfaces keep the (1 - roughness) damping. Removing it wholesale when the probe
        // landed was wrong, and the road said so: asphalt is enormous, flat and viewed almost
        // edge-on, so EnvBRDFApprox's grazing Fresnel rise swept a bright band down the carriageway.
        // The probe fixed the COLOUR of that reflection - a rough road now samples dim ground rather
        // than bright sky - but not its strength at grazing angles, which is what produced the
        // artefact.
        float3 env = texCUBElod(g_EnvCube, float4(R, roughness * g_EnvInfo.x)).rgb;

        indirect += env * EnvBRDFApprox(f0, roughness, NdotV) * g_EnvInfo.y * (1.0f - roughness);
    }
    else
    {
        // No probe could be created: the flat hemisphere term, as before. Worse looking, still
        // correct.
        indirect += irradiance * EnvBRDFApprox(f0, roughness, NdotV) * (1.0f - roughness);
    }

    float3 color = direct + indirect;

    // Emissive, driven by agiTexProp::AlphaGlow / Lightmap - so signs, lit windows and glows keep
    // their punch through the tonemap instead of being crushed along with everything else.
    color += albedo * g_Material.z;
#else
    // Unlit permutation: vertex colours are already final (FirstPass() would have drawn them
    // as-is), and `albedo` above already includes them. Reproduce that directly.
    float3 color = albedo;
#endif

    color *= g_CameraPos.w; // exposure

    color = ApplyFog(color, i.WorldPos, view_dist);

    // Tonemapping is opt-in (g_Ambient.w, from -d3d9tonemap), and off by default.
    //
    // ACES is the right tool for genuinely HDR input, and this path will want it once there are
    // enough real light sources to push the sum well past 1.0. It is the wrong tool right now: with
    // the light rig feeding values the engine authored as 0..1 multipliers, the curve's shoulder
    // pulls midtones *down* and takes contrast out of an image that had no excess range to
    // reclaim - which compounded the 1/PI error above into the flat, grey look. Default to a plain
    // clamp so the baseline is honest, and let the curve be switched on when it has something to do.
    color = (g_Ambient.w > 0.5f) ? TonemapACES(color) : saturate(color);

    return float4(LinearToSrgb(color), alpha);
}

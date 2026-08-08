//  Open1560 - Pathway B world vertex shader (vs_3_0)
//
//  Physically-based replacement for the D3D9 fixed-function pipeline on the world-space
//  (agiRasterizer::MeshWorld) path. See docs/dx9_rendering_pathways.md.
//
//  Compiled at runtime by agidx9/dx9shader.cpp via D3DCompile, so this file can be edited and the
//  game relaunched without rebuilding any C++.
//
//  Split from the pixel shader because D3D9 gives each stage its own constant register file, and a
//  single translation unit declaring explicit register() bindings for both would collide. The GL
//  backend splits its shaders the same way (agigl: .vs / .fs).
//
//  Positions arrive in MODEL space. Lighting is done in WORLD space by the pixel shader - not view
//  space as the FF pipeline does - so the light rig stays in the engine's own coordinates and none
//  of MeshWorld()'s view-matrix z-flip has to be reasoned about again downstream.

float4x4 g_WorldViewProj : register(c0);  // c0..c3
float4x4 g_World         : register(c4);  // c4..c7
float3x3 g_WorldIT       : register(c8);  // c8..c10

struct VSOut
{
    float4 Position : POSITION;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
    float3 Normal   : TEXCOORD2;
    float4 Color    : COLOR0;
};

VSOut main(float3 pos : POSITION, float3 normal : NORMAL, float4 color : COLOR0, float2 uv : TEXCOORD0)
{
    VSOut o;

    o.Position = mul(float4(pos, 1.0f), g_WorldViewProj);
    o.WorldPos = mul(float4(pos, 1.0f), g_World).xyz;

    // Normals go through the SAME matrix as positions.
    //
    // This previously used a separately-uploaded inverse-transpose (g_WorldIT - dx9shader.cpp still
    // uploads it at c8..c10, but nothing reads it now). It was changed while chasing a "up-facing
    // surfaces render black" bug, on a misreading of a normal-visualisation capture. That was NOT
    // the cause: the real cause was that the capture came from the vehicle-select screen, which
    // never loads the city and so never runs fix_sun() - sampling the sun direction there gave
    // (0.137, 0.004, -0.992), i.e. a default pointing along -Z with *zero* vertical component, and
    // MM1 is Y-up (mmphysics/inertia.h: Gravity {0, -10, 0}). A horizontal sun gives NdotL == 0 on
    // every up-facing surface, which is the black tops, and it has nothing to do with normals.
    //
    // The change is kept because it is the simpler and safer of the two, not because it fixed
    // anything: g_World's constant-register packing is *empirically* correct (positions transform
    // correctly through it), whereas the hand-built cofactor upload depended on my own reasoning
    // about float3x3 packing that I never verified. For rotations and uniform scale - what these
    // instances carry - the two agree up to a scale factor and the pixel shader normalises anyway.
    // It also matches what fixed-function does with normals, which Pathway A renders correctly.
    //
    // Neither form handles genuine non-uniform scale, which would shear normals off their surface.
    // The fixed-function path has the same limitation (it leans on D3DRS_NORMALIZENORMALS), so this
    // is not a regression - but it is what to revisit if a non-uniformly scaled instance shades
    // oddly. If that day comes, fix the cofactor upload rather than re-deriving it here.
    o.Normal = mul(normal, (float3x3) g_World);

    o.UV = uv;
    o.Color = color;

    return o;
}

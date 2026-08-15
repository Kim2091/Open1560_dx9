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

define_dummy_symbol(mmsettings_settings);

#include "settings.h"

#include <cstring>

#define TOGGLE(NAME, PAGE, LABEL, DEFAULT, HELP) \
    {NAME, LABEL, mmSettingPage::PAGE, mmSettingKind::Toggle, DEFAULT, DEFAULT, 0.0f, 1.0f, 0, HELP}

#define RATIO(NAME, PAGE, LABEL, DEFAULT, MIN, MAX, HELP) \
    {NAME, LABEL, mmSettingPage::PAGE, mmSettingKind::Ratio, DEFAULT, DEFAULT, MIN, MAX, 0, HELP}

#define COUNT(NAME, PAGE, LABEL, DEFAULT, MIN, MAX, HELP) \
    {NAME, LABEL, mmSettingPage::PAGE, mmSettingKind::Count, DEFAULT, DEFAULT, MIN, MAX, 0, HELP}

// The table.
//
// The defaults here MUST match the ones the consumers pass to get_or(), because those calls are what
// this is initialised from and a disagreement would mean the menu showed one thing while the game
// did another. Where a consumer has been converted to read live, its get_or() default is gone and
// this is the only copy left, which is the end state for all of them.
static mmSetting SettingsTable[] {
    // --- Graphics ---------------------------------------------------------------------------
    COUNT("aniso", Graphics, "Anisotropic Filtering", 16.0f, 1.0f, 16.0f,
        "Sharpens surfaces seen at a glancing angle. The road runs to the horizon in almost every "
        "frame, which is exactly the case trilinear filtering handles worst."),
    TOGGLE("smoothnormals", Graphics, "Smooth Normals", 1.0f,
        "Averages each vertex normal over the faces that meet there, so low-poly bodywork stops "
        "looking faceted."),
    TOGGLE("flatnormals", Graphics, "Faceted Shading", 0.0f,
        "Throws away the stored normals and shades from the geometry instead. A real change in look, "
        "and better only where the stored normals are bad."),
    RATIO("reflectamount", Graphics, "Vehicle Reflections", 0.35f, 0.0f, 1.0f,
        "How strongly the sphere map shows on car paint."),
    RATIO("reflectfresnelscale", Graphics, "Reflection Fresnel", 0.0f, 0.0f, 1.0f,
        "Strengthens reflections at grazing angles, the way real paint behaves. The original has no "
        "trace of this, so it is off by default."),
    TOGGLE("trafficrefl", Graphics, "Traffic Reflections", 0.0f,
        "Gives AI and traffic vehicles the same sphere-map paint the player's car has."),
    RATIO("reflectspecular", Graphics, "Reflection Glint", 0.0f, 0.0f, 1.0f,
        "Adds a sun glint on top of the reflection. Also an embellishment the original does not have."),
    TOGGLE("d3d9nofx", Graphics, "Skip Second Passes", 0.0f,
        "Drops the chrome and ground reflection passes. They cost real visuals and only earn their "
        "keep when capturing for RTX Remix, which does reflections itself."),

    // --- Lighting ---------------------------------------------------------------------------
    TOGGLE("ffperpixel", Lighting, "Per-Pixel Sun", 0.0f,
        "Evaluates the sun per fragment instead of per vertex, so light follows the surface rather "
        "than the triangle edges. Costs one extra pass per mesh."),
    TOGGLE("d3d9specular", Lighting, "City Specular", 0.0f,
        "Lets the fixed city lighting produce highlights. The original rig has no specular concept at "
        "all, so this is an addition rather than a restoration."),
    RATIO("glowreachscale", Lighting, "Light Glow Reach", 14.0f, 4.0f, 40.0f,
        "How far light spills from a glow, relative to the size of the flare that produced it."),
    RATIO("lightlamp", Lighting, "Street Lamp Brightness", 1.0f, 0.0f, 4.0f,
        "Intensity of street lamps and other static lights."),
    RATIO("lightvehicle", Lighting, "Vehicle Light Brightness", 1.0f, 0.0f, 4.0f,
        "Intensity of tail, brake and reverse lights."),
    RATIO("lighthead", Lighting, "Headlight Brightness", 1.0f, 0.0f, 4.0f, "Intensity of headlight cones."),
    RATIO("lighttraffic", Lighting, "Traffic Light Brightness", 1.0f, 0.0f, 4.0f, "Intensity of traffic signals."),
    TOGGLE("glowstreetlamps", Lighting, "Street Lamps Light", 1.0f, "Street lamps cast light, not just a flare."),
    TOGGLE("glowvehiclelights", Lighting, "Vehicle Lights Light", 1.0f, "Tail and brake lights cast light."),
    TOGGLE("glowheadlights", Lighting, "Headlights Light", 1.0f, "Headlight cones cast light."),
    TOGGLE("glowtrafficlights", Lighting, "Traffic Lights Light", 1.0f, "Traffic signals cast light."),

    // --- Camera -----------------------------------------------------------------------------
    RATIO("orbitshakeamp", Camera, "Camera Shake", 1.0f, 0.0f, 2.0f,
        "Overall strength of the shake, in every camera that has one."),
    RATIO("orbitshake", Camera, "Engine Shake", 0.8f, 0.0f, 2.0f,
        "How much the engine contributes. This is the source that rises with the revs."),
    RATIO("orbitshakerough", Camera, "Road Shake", 0.55f, 0.0f, 2.0f,
        "How much rough road contributes, measured from the suspension."),
    RATIO("orbitshakeimpact", Camera, "Impact Shake", 1.0f, 0.0f, 2.0f, "How hard a collision hits the camera."),
    RATIO("orbitshakeaccel", Camera, "Acceleration Shake", 0.35f, 0.0f, 2.0f,
        "How much hard acceleration and braking contribute."),
    RATIO("orbitshaketurn", Camera, "Steady In Turns", 1.0f, 0.0f, 1.0f,
        "How much of the shake stands down while the view is being turned. Vibration on a view that "
        "is already moving reads as instability rather than speed."),
    COUNT("orbitshakemingear", Camera, "Shake From Gear", 3.0f, 1.0f, 6.0f,
        "Lowest gear the engine shake is allowed in. Shaking through the short lower gears turns "
        "every pull-away into a rattle."),
    RATIO("orbitshakerpm", Camera, "Shake From Revs", 0.55f, 0.0f, 1.0f,
        "Fraction of the redline the engine shake starts building from."),
    RATIO("povbob", Camera, "In-Car Head Bob", 1.0f, 0.0f, 2.0f,
        "How much the driver's head moves in the seat, and how far it leans under acceleration."),
    RATIO("povbobsway", Camera, "In-Car Head Sway", 1.0f, 0.0f, 2.0f,
        "How much the head bob rotates the view, as opposed to moving it."),
    TOGGLE("povlook", Camera, "In-Car Free Look", 1.0f,
        "Look around the cabin with the mouse. Turn this off if you steer with the mouse."),
    RATIO("povlooksens", Camera, "Free Look Speed", 0.0035f, 0.0005f, 0.0120f,
        "Radians of view movement per unit of mouse motion."),
    TOGGLE("orbitcollide", Camera, "Orbit Camera Collision", 1.0f,
        "Keeps the orbital camera out of roads, walls and hillsides instead of letting the view pass "
        "through them."),
    RATIO("orbitdist", Camera, "Orbit Distance", 6.0f, 2.0f, 15.0f,
        "How far the orbital camera sits behind the car at a standstill."),
    RATIO("orbitfov", Camera, "Speed FOV Widening", 18.0f, 0.0f, 40.0f,
        "Degrees the field of view opens by at full speed."),
    RATIO("orbitsens", Camera, "Orbit Mouse Speed", 0.0045f, 0.0005f, 0.0150f,
        "Radians of camera movement per unit of mouse motion."),

    // --- Debug ------------------------------------------------------------------------------
    TOGGLE("nocull", Debug, "Disable Culling", 0.0f,
        "Draws back faces, all LODs and everything past the draw distance. For RTX Remix captures, "
        "which need closed geometry - a back face never submitted is a hole light leaks through."),
    TOGGLE("ghash", Debug, "Report Hash Churn", 0.0f,
        "Counts how many geometry hashes RTX Remix would see minted per frame. A steady scene that "
        "keeps producing new ones cannot hold a mesh replacement."),
    TOGGLE("ghashcolor", Debug, "Hash Colour View", 0.0f,
        "Tints every world draw by its geometry hash. Stable geometry holds its colour; anything "
        "rehashed per frame strobes."),
    TOGGLE("noskin", Debug, "Disable GPU Skinning", 0.0f,
        "Submits pedestrians one bone per draw instead of through a matrix palette. Slower, but it "
        "gives RTX Remix an unambiguous world matrix per submission."),
    TOGGLE("pedskin", Debug, "CPU Pedestrian Skinning", 0.0f,
        "Skins pedestrians on the CPU, as the original did. Breaks hash stability outright - the "
        "positions then change every frame."),
    TOGGLE("nativecpucull", Debug, "CPU Backface Cull", 0.0f,
        "Restores the camera-dependent backface test. It makes the submitted index list depend on "
        "where you are standing, which is why it is off."),
    TOGGLE("d3d9depthbias", Debug, "Depth Bias", 0.0f,
        "Pulls hardware-transformed geometry towards the camera. Left over from when the road was "
        "still drawn on the CPU; it now biases world geometry against everything that is not."),
};

#undef TOGGLE
#undef RATIO
#undef COUNT

static constexpr i32 MaxChangeCallbacks = 8;

static mmSettingsChangeCallback ChangeCallbacks[MaxChangeCallbacks] {};
static i32 ChangeCallbackCount = 0;

// Starts at 1 so that a consumer whose stored generation is zero-initialised always resolves once.
static u32 SettingsGeneration = 1;

mmSetting* mmSettingsAll(i32& count)
{
    count = static_cast<i32>(ARTS_SIZE(SettingsTable));

    return SettingsTable;
}

mmSetting* mmSettingFind(const char* name)
{
    for (mmSetting& setting : SettingsTable)
    {
        if (!std::strcmp(setting.Name, name))
            return &setting;
    }

    return nullptr;
}

bool mmSettingBool(const char* name, bool fallback)
{
    const mmSetting* setting = mmSettingFind(name);

    return setting ? (setting->Value >= 0.5f) : fallback;
}

f32 mmSettingFloat(const char* name, f32 fallback)
{
    const mmSetting* setting = mmSettingFind(name);

    return setting ? setting->Value : fallback;
}

i32 mmSettingInt(const char* name, i32 fallback)
{
    const mmSetting* setting = mmSettingFind(name);

    return setting ? static_cast<i32>(setting->Value + 0.5f) : fallback;
}

void mmSettingsInit()
{
    // Seeded from the command line, which is what keeps every existing invocation working unchanged -
    // and keeps it winning, since this runs before anything can read the table.
    //
    // The parameters are found by walking the registry rather than by constructing one per setting.
    // A cmd_param built here would be a SECOND registration of a name the consuming code already
    // owns, and worse, it would be built long after cmd_param::init() walked argv - so it would
    // silently never receive its value and every setting would read as its default no matter what
    // was passed. That failure mode is documented at the top of agidx9/dx9rsys.cpp, which was bitten
    // by the same thing.
    mem::cmd_param* params[512];

    const std::size_t found = mem::cmd_param::collect(params, ARTS_SIZE(params));
    const std::size_t count = (found < ARTS_SIZE(params)) ? found : ARTS_SIZE(params);

    if (found > ARTS_SIZE(params))
        Errorf("mmSettingsInit: %zu parameters registered, only %zu collected", found, count);

    for (mmSetting& setting : SettingsTable)
    {
        setting.Value = setting.Default;

        for (std::size_t i = 0; i < count; ++i)
        {
            if (std::strcmp(params[i]->name(), setting.Name))
                continue;

            if (setting.Kind == mmSettingKind::Toggle)
                setting.Value = params[i]->get_or(setting.Default >= 0.5f) ? 1.0f : 0.0f;
            else
                setting.Value = params[i]->get_or(setting.Default);

            break;
        }
    }

    mmSettingsSync();
}

void mmSettingsSync()
{
    for (mmSetting& setting : SettingsTable)
    {
        setting.Value = std::clamp(setting.Value, setting.Min, setting.Max);

        // The integer mirror is rounded rather than truncated. A slider that has been dragged to
        // what looks like 4 can easily be holding 3.9999, and truncating that to 3 is the kind of
        // fault nobody thinks to look for.
        setting.Integer = static_cast<i32>(setting.Value + ((setting.Value >= 0.0f) ? 0.5f : -0.5f));
    }

    ++SettingsGeneration;

    for (i32 i = 0; i < ChangeCallbackCount; ++i)
        ChangeCallbacks[i]();
}

u32 mmSettingsGeneration()
{
    return SettingsGeneration;
}

void mmSettingsRestoreDefaults(mmSettingPage page, bool all)
{
    for (mmSetting& setting : SettingsTable)
    {
        if (all || (setting.Page == page))
            setting.Value = setting.Default;
    }

    mmSettingsSync();
}

void mmSettingsOnChange(mmSettingsChangeCallback callback)
{
    if (!callback)
        return;

    for (i32 i = 0; i < ChangeCallbackCount; ++i)
    {
        if (ChangeCallbacks[i] == callback)
            return;
    }

    if (ChangeCallbackCount < MaxChangeCallbacks)
        ChangeCallbacks[ChangeCallbackCount++] = callback;
    else
        Errorf("mmSettingsOnChange: raise MaxChangeCallbacks past %d", MaxChangeCallbacks);
}

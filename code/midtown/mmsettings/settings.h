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

// The live settings for everything this project has added on top of the original game - the D3D9
// renderer, the cameras, and the modding switches - in one table, so they can be presented in a menu
// and changed while the game is running.
//
// WHY THIS EXISTS AT ALL. Every one of these started as a mem::cmd_param, which is exactly right for
// something you decide before launching and never touch again: it is read once, usually into a local
// at init, and the process lives with the answer. A menu is the opposite. It needs the value to be
// somewhere it can be pointed at, changed, read back, and reverted if the player cancels - and a
// command-line string parsed into a local variable is none of those things.
//
// So each setting keeps a LIVE value here, seeded from its command-line parameter at startup. That
// ordering matters and is the whole compatibility story: a command line that worked before still
// works and still wins, because it is what the table is initialised from. Nothing changes for anyone
// who does not open the menu.
//
// The consumers read through the accessors below rather than caching, so a change takes effect on
// the next frame without anything having to be told about it. Where a consumer genuinely cannot do
// that - the cameras resolve a dozen tunables into their own fields when a race starts - it
// registers a callback with mmSettingsOnChange instead.

#include "vector7/vector3.h"

enum class mmSettingKind
{
    // A checkbox. Stored as an integer 0 or 1, because that is what UIToggleButton2 binds to.
    Toggle,

    // A slider over a continuous range.
    Ratio,

    // A slider over a whole-numbered range - counts, levels, indices.
    Count,
};

// Which page a setting belongs on.
//
// The split is not by subsystem but by AUDIENCE. Visual settings change what the game looks like and
// belong in front of anyone who opens the options; diagnostics answer questions about the renderer -
// what it is submitting, whether a hash is stable, which path a draw took - and in front of a player
// they are at best noise and at worst a way to make the game look broken with no idea why. So they
// live behind the debug page, which the game already gates on Sim()->IsDebug().
enum class mmSettingPage
{
    Graphics,
    Lighting,
    Camera,
    Debug,
};

struct mmSetting
{
    // The command-line parameter this shadows, without the leading dash. Also the key it is saved
    // under, so a settings file and a command line name the same things the same way.
    const char* Name {};

    // What the menu shows. Kept short - it shares a row with the control.
    const char* Label {};

    mmSettingPage Page {};
    mmSettingKind Kind {};

    // The live value, and what to put back when the player asks for defaults. Toggles and counts use
    // whole numbers in Value all the same; there is one storage type so that one table can describe
    // every setting, and the widgets that need an i32* are handed Integer instead.
    f32 Value {};
    f32 Default {};

    f32 Min {};
    f32 Max {};

    // Integer mirror, for the widgets that bind to an i32* rather than an f32*. Kept in step with
    // Value by mmSettingsSync; nothing else should write it.
    i32 Integer {};

    // One line of explanation, shown under the row. Worth having: several of these have no visible
    // effect until you know what to look at.
    const char* Help {};
};

// The whole table, and its length.
mmSetting* mmSettingsAll(i32& count);

// One setting by name, or null. The name is the command-line spelling.
mmSetting* mmSettingFind(const char* name);

// Reads a setting. These are what the consumers call, every time they need the value, rather than
// caching it - that is what makes a menu change take effect without anything being notified.
//
// A name that is not in the table returns the fallback, so a caller is never left guessing whether
// it got a real answer.
bool mmSettingBool(const char* name, bool fallback = false);
f32 mmSettingFloat(const char* name, f32 fallback = 0.0f);
i32 mmSettingInt(const char* name, i32 fallback = 0);

// Seeds every setting from its command-line parameter. Call once, early - before anything reads a
// setting, and before the menus are built.
void mmSettingsInit();

// Brings the integer mirrors back in step with the values and runs the change callbacks. Call after
// writing to a setting's Value directly, which is what the menu widgets do.
void mmSettingsSync();

// Puts one page, or every page when `all` is set, back to defaults.
void mmSettingsRestoreDefaults(mmSettingPage page, bool all = false);

// Registered by a consumer that cannot read its settings live - see the note at the top. Called on
// every mmSettingsSync, so it must be cheap and safe to run when nothing it cares about changed.
using mmSettingsChangeCallback = void (*)();

void mmSettingsOnChange(mmSettingsChangeCallback callback);

// Bumped by every mmSettingsSync. A consumer that resolves a group of settings into its own fields
// keeps the value it last resolved at and re-reads when it differs - one integer compare per frame
// instead of a dozen name lookups, and without needing a callback that can reach every instance of
// itself. See mmCamShake, which is exactly that case.
u32 mmSettingsGeneration();

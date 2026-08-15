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

define_dummy_symbol(mmui_pu_graphics);

#include "pu_graphics.h"

#include "arts7/sim.h"
#include "mmcityinfo/playercfg.h"
#include "mmwidget/manager.h"

#include "pu_advanced.h"
#include "pu_audio.h" // PUOptionsConfig

// ?fix_lighting_lame@@YAXXZ
ARTS_IMPORT /*static*/ void fix_lighting_lame();

// ?toggle_filter@@YAXXZ
ARTS_IMPORT /*static*/ void toggle_filter();

// ?toggle_interlace@@YAXXZ
ARTS_IMPORT /*static*/ void toggle_interlace();

// Widget id for the row this adds. Past the range the original dispatches on - see the note in
// pu_advanced.cpp.
static constexpr i32 IDC_GRAPHICS_ADVANCED = 382;

// The original, plus a way through to the settings this project added.
//
// PUGraphics::PreSetup is one statement in the original (game.asm ~211176) - it reloads the graphics
// options into the page's widgets - and that is reproduced exactly below. Everything the page
// already does is untouched; this only appends a button.
//
// The advanced pages are built here rather than at startup because a UIMenu registers itself with
// the MenuManager when it is constructed, and the manager is torn down and rebuilt between races.
// Creating them on the way into the page they are reached from is what keeps them present whenever
// they can actually be opened.
void PUGraphics::PreSetup()
{
    PUOptionsConfig->GetGraphics();

    // Diagnostics are shown only in a debug session, on the same test the pause menu already uses to
    // decide whether to offer its Debug page. In front of a player they are at best noise and at
    // worst a way to make the game look broken with no idea why.
    const i32 first = mmAdvancedMenuEnsure(Sim()->IsDebug());

    // Widgets added here persist on the menu, so this must only happen once. PreSetup runs on every
    // open.
    if (FindWidget(IDC_GRAPHICS_ADVANCED))
        return;

    AddButton(IDC_GRAPHICS_ADVANCED, LOC_TEXT("Advanced..."), 0.0f, 0.85f, 0.45f, widget_height_, widget_font_size_, 2,
        Callback {[first] { MenuMgr()->Switch(first); }});
}

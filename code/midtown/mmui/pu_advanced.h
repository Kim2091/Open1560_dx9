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

#include "mmsettings/settings.h"
#include "pu_menu.h"

// The pause-menu pages for everything this project added - see mmsettings/settings.h for the table
// they are built from and why it exists.
//
// One page holds a fixed number of rows and links to the next, because a UIMenu's widgets are built
// in its constructor and the settings table is longer than a screen. Chaining several small menus is
// what avoids having to add and remove widgets at runtime, which nothing else in this UI does.
//
// These are not in the original game, so like OrbitCamCS this class declares no imported or exported
// members, no GetClass() override, and no check_size - nothing reflects on it, and its size is
// unconstrained.
class PUAdvanced final : public PUMenuBase
{
public:
    // first/count select this page's slice of the settings table. next_menu_id is the page the
    // "More" button leads to, or -1 for the last page in a chain.
    PUAdvanced(i32 menu_id, mmSettingPage page, i32 first, i32 count, i32 next_menu_id, const char* title);

    ~PUAdvanced() override = default;

    // Pushes whatever the widgets have written back through mmSettingsSync, so a change is live
    // while the menu is still open rather than on the way out - which is the whole point of a
    // settings menu you can see the game behind.
    void Update() override;

private:
    mmSettingPage page_ {};
};

// Menu ids for the pages above, continuing past IDD_PU_DEBUG (12) in mmgame/popup.h.
//
// Deliberately well clear of it: the ids the original game dispatches on are a dense range it
// switches over, and starting at 20 leaves room for that range to grow without a silent collision.
enum
{
    IDD_PU_ADV_FIRST = 20,

    // Enough ids for every page the table can produce. Five rows to a page over forty-odd settings
    // needs eleven; the slack is so that adding a setting does not silently drop a page off the end.
    IDD_PU_ADV_COUNT = 20,
};

// Creates the pages if they are not already registered, and returns the id of the first one.
//
// Called from the graphics page rather than at startup, because a UIMenu registers itself with the
// MenuManager on construction and the manager is torn down and rebuilt between races - so the pages
// have to be (re)created whenever they are next needed, not once.
i32 mmAdvancedMenuEnsure(bool debug_pages);

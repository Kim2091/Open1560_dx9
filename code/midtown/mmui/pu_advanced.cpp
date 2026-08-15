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

define_dummy_symbol(mmui_pu_advanced);

#include "pu_advanced.h"

#include "mmwidget/manager.h"

// Widget ids for this page's rows. The originals run 10..101 and are dispatched through a jump table
// in closed code (mmPopup::Update), so these start well past that: a row here must not land on an id
// the game already means something by.
enum
{
    IDC_ADV_ROW = 400,
    IDC_ADV_MORE = 380,
    IDC_ADV_DEFAULTS = 381,
};

// How many settings one page shows. Bounded by the menu's height rather than by taste - the rows are
// laid out from the title down and have to fit above the exit button.
static constexpr i32 AdvancedRowsPerPage = 9;

// The right-hand column, matching what the original graphics page uses for its own controls, so the
// two pages line up if the player moves between them.
static constexpr f32 AdvancedControlX = 0.55f;
static constexpr f32 AdvancedControlWidth = 0.4f;

static constexpr i32 AdvancedFontSize = 24;

PUAdvanced::PUAdvanced(i32 menu_id, mmSettingPage page, i32 first, i32 count, i32 next_menu_id, const char* title)
    : PUMenuBase(menu_id, 0.0f, 0.0f, 0.0f, 0.0f, nullptr)
    , page_(page)
{
    AssignName(LOC_TEXT(title));

    const f32 top = CreateTitle();

    AddExit(0.65f, 0.0f, 0.35f, 0.075f);

    i32 total = 0;
    mmSetting* settings = mmSettingsAll(total);

    const f32 step = widget_height_ * 1.15f;

    f32 y = top;

    for (i32 i = 0; i < count; ++i)
    {
        mmSetting& setting = settings[first + i];

        // The row id is only ever used to find the widget again; nothing dispatches on it.
        const i32 idc = IDC_ADV_ROW + first + i;

        if (setting.Kind == mmSettingKind::Toggle)
        {
            // Bound to the integer mirror, because that is what a toggle writes. mmSettingsSync
            // copies it back to the value the game actually reads.
            AddToggle2(idc, LOC_TEXT(setting.Label), &setting.Integer, AdvancedControlX, y, AdvancedControlWidth,
                widget_height_, AdvancedFontSize, 1, Callback {});
        }
        else
        {
            // Bound to the value directly. The trailing four arguments are the ones the original
            // graphics page passes for its own sliders (game.asm ~191161); they are not documented
            // anywhere and copying a known-good set is better than guessing at their meaning.
            AddSlider(idc, LOC_TEXT(setting.Label), &setting.Value, AdvancedControlX, y, AdvancedControlWidth,
                widget_height_, setting.Min, setting.Max, 11, 0, AdvancedFontSize, 0, Callback {});
        }

        y += step;
    }

    // Defaults applies to this page only. Restoring everything from one button is a bigger action
    // than it looks - it would silently undo a camera the player spent a while tuning because they
    // wanted to reset the renderer.
    const mmSettingPage reset_page = page;

    AddButton(IDC_ADV_DEFAULTS, LOC_TEXT("Restore Defaults"), 0.0f, y, 0.45f, widget_height_, AdvancedFontSize, 2,
        Callback {[reset_page] { mmSettingsRestoreDefaults(reset_page); }});

    if (next_menu_id >= 0)
    {
        const i32 next = next_menu_id;

        AddButton(IDC_ADV_MORE, LOC_TEXT("More..."), 0.5f, y, 0.45f, widget_height_, AdvancedFontSize, 2,
            Callback {[next] { MenuMgr()->Switch(next); }});
    }

    SetBstate(0);
}

void PUAdvanced::Update()
{
    PUMenuBase::Update();

    // Every frame the page is open. mmSettingsSync only bumps its generation and runs the change
    // callbacks when a value has actually moved, so the common case - a page sitting open with
    // nothing being dragged - costs one pass over the table and nothing else.
    mmSettingsSync();
}

i32 mmAdvancedMenuEnsure(bool debug_pages)
{
    // Already built for this menu manager. The manager is torn down between races and rebuilt, and
    // UIMenu registers itself on construction, so this is the check that decides whether the pages
    // exist right now rather than whether they were ever created.
    if (MenuMgr()->FindMenu(IDD_PU_ADV_FIRST) >= 0)
        return IDD_PU_ADV_FIRST;

    struct PageSpec
    {
        mmSettingPage Page;
        const char* Title;
    };

    static constexpr PageSpec Pages[] {
        {mmSettingPage::Graphics, "Advanced Graphics"},
        {mmSettingPage::Lighting, "Advanced Lighting"},
        {mmSettingPage::Camera, "Camera"},
        {mmSettingPage::Debug, "Renderer Diagnostics"},
    };

    i32 total = 0;
    const mmSetting* settings = mmSettingsAll(total);

    // The table is grouped by page, so a page's settings are one contiguous run. Finding the runs
    // rather than assuming their bounds keeps this correct when a setting is added or moved.
    struct Chunk
    {
        mmSettingPage Page;
        const char* Title;
        i32 First;
        i32 Count;
    };

    Chunk chunks[IDD_PU_ADV_COUNT] {};
    i32 chunk_count = 0;

    for (const PageSpec& spec : Pages)
    {
        if ((spec.Page == mmSettingPage::Debug) && !debug_pages)
            continue;

        i32 first = -1;
        i32 count = 0;

        for (i32 i = 0; i < total; ++i)
        {
            if (settings[i].Page != spec.Page)
                continue;

            if (first < 0)
                first = i;

            ++count;
        }

        // Split into as many pages as the run needs. A page that would hold a single trailing row is
        // still its own page; balancing them would mean the rows moving about as settings are added,
        // and a stable position is worth more than a tidy last page.
        for (i32 taken = 0; taken < count; taken += AdvancedRowsPerPage)
        {
            if (chunk_count >= IDD_PU_ADV_COUNT)
            {
                Errorf("mmAdvancedMenuEnsure: raise IDD_PU_ADV_COUNT past %d", IDD_PU_ADV_COUNT);
                break;
            }

            chunks[chunk_count++] = {
                spec.Page, spec.Title, first + taken, std::min(AdvancedRowsPerPage, count - taken)};
        }
    }

    // Built last-first so each page can be handed the id of the one after it.
    for (i32 i = chunk_count - 1; i >= 0; --i)
    {
        const bool has_next = (i + 1) < chunk_count;

        // Leaked deliberately - and it is not a leak. UIMenu's constructor registers the menu with
        // the MenuManager, which adopts it as a child node (see AddMenu2 in game.asm), so the
        // manager owns it from that moment and destroys it with the rest of its children. Holding a
        // second owning pointer here is what would be wrong.
        new PUAdvanced(IDD_PU_ADV_FIRST + i, chunks[i].Page, chunks[i].First, chunks[i].Count,
            has_next ? (IDD_PU_ADV_FIRST + i + 1) : -1, chunks[i].Title);
    }

    return IDD_PU_ADV_FIRST;
}

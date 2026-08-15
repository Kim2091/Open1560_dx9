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

define_dummy_symbol(mmui_pu_menu);

#include "pu_menu.h"

#include "agi/bitmap.h"
#include "agi/pipeline.h"
#include "mmeffects/mmtext.h"
#include "mmwidget/manager.h"

PUMenuBase::PUMenuBase(
    i32 menu_id, [[maybe_unused]] f32 x, [[maybe_unused]] f32 y, f32 width, f32 height, char* background)
    : UIMenu(menu_id)
{
    if (background)
    {
        // Both branches ask for the background at the size of the UI area rather than at the size it
        // was authored, which is what makes the menu fill the screen at any resolution.
        //
        // The scale arguments are a FRACTION OF THE UI AREA, not of the window: agiBitmap::BeginGfx
        // resolves them as UI_Width/UI_Height * scale and reloads the image resampled to that. Zero
        // means "keep the file's own size", and that is what the flat branch used to pass - so a
        // 640x480 background stayed 640x480 while the widgets laid over it were positioned in
        // normalised coordinates across the whole UI area. At 1920x1080 (UI area 1440x1080, from the
        // UI Position line in the log) that is a third-size picture under full-size controls, which
        // is the "menu is stuck at 480p" this fixes.
        //
        // The art is upscaled rather than redrawn, so it is softer than it was at 640x480. That is
        // inherent in 4:3 assets from 1999 and is the better of the two failures.
        const i32 flags = MenuMgr()->Is3D() ? 0 : BITMAP_TRANSPARENT;

        bg_bitmap_ = as_rc Pipe()->GetBitmap(background, 1.0f, 1.0f, flags);

        if (!bg_bitmap_)
        {
            bg_bitmap_ = as_rc CreateDummyBitmap();

            ArAssert(bg_bitmap_, "Could not create backgrond");
        }

        menu_width_ = static_cast<f32>(bg_bitmap_->GetWidth()) / static_cast<f32>(Pipe()->GetWidth());
        menu_height_ = static_cast<f32>(bg_bitmap_->GetHeight()) / static_cast<f32>(Pipe()->GetHeight());
    }
    else if (width > 0.0f && height > 0.0f)
    {
        width *= UI_ScaleX;
        height *= UI_ScaleY;

        menu_width_ = width;
        menu_height_ = height;
    }

    menu_x_ = UI_StartX + (UI_ScaleX - menu_width_) / 2.0f;
    menu_y_ = UI_StartY + (UI_ScaleY - menu_height_) / 2.0f;

    Pipe()->RoundNormalized(menu_x_, menu_y_);

    if (bg_bitmap_)
    {
        bg_x_ = std::lround(menu_x_ * Pipe()->GetWidth());
        bg_y_ = std::lround(menu_y_ * Pipe()->GetHeight());
    }

    field_AC = 0.075f;
    widget_height_ = 0.1f;
    field_68 = 1;
    field_BC = 0.1f;
    field_B4 = 0.9f;
    widget_font_size_ = MenuMgr()->GetFieldD0() ? 24 : 32;
    field_B0 = 0.5f;
    field_B8 = 0.5f;
}

void PUMenuBase::Cull()
{
    i32 x = bg_x_;
    i32 y = bg_y_;
    i32 width = bg_bitmap_->GetWidth();
    i32 height = bg_bitmap_->GetHeight();

    Pipe()->CopyBitmap(x, y, bg_bitmap_.get(), 0, 0, width, height);

    if (MenuMgr()->Is3D())
        Pipe()->ClearBorder(x, y, width, height, 0);
}

Ptr<mmTextNode> PUMenuBase::CreateTextNode(f32 x, f32 y, f32 width, f32 height, i32 lines, i32 flags)
{
    Ptr<mmTextNode> result = arnew mmTextNode();
    result->Init(menu_x_ + (x * menu_width_), menu_y_ + (y * menu_height_), width * menu_width_, height * menu_height_,
        lines, flags);
    return result;
}

i32 PUMenuBase::AddText(mmTextNode* node, void* font, LocString* text, i32 effects, f32 x, f32 y)
{
    return node->AddText(font, text, effects, x * menu_width_, y * menu_height_);
}

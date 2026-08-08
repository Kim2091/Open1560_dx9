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

// arts_pch.h unconditionally defines DIRECT3D_VERSION 0x0600, for the legacy (DX6) <d3d.h>
// used elsewhere in the project (vendor/dx6). <d3d9.h> from a modern Windows SDK checks that
// same macro to decide whether to declare its own (0x0900) interfaces/structures at all
// (IDirect3D9, D3DCAPS9, Direct3DCreate9, ...) - with the stale 0x0600 value already in place,
// it silently declares nothing. Undefine it right before including <d3d9.h> so the header's own
// "#if !defined(DIRECT3D_VERSION)" default kicks in; this only changes which SDK declarations
// are visible in this module's translation units, not anything about <d3d.h>'s own usage.
#include "core/minwin.h"

#undef DIRECT3D_VERSION
#include <d3d9.h>

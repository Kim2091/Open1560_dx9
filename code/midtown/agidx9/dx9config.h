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

// Loads Open1560-Shaders.ini, the tuning file for the programmable rendering path (Pathway B) and
// the glow-driven lighting that feeds it. See docs/dx9_rendering_pathways.md.
//
// It does not introduce a settings system of its own: every key is simply an existing
// mem::cmd_param, applied through mem::cmd_param::set() exactly as a command-line switch would be.
// That keeps one source of truth for defaults and means anything tunable on the command line is
// tunable from the file and vice versa.
//
// MUST be called before mem::cmd_param::init(argc, argv). Later assignments overwrite earlier ones,
// so loading first is what gives the command line precedence over the file - the right way round,
// since the file is a persistent preference and the command line is a deliberate one-off override.
//
// Writes a fully commented template on first run if the file is absent, so the available knobs are
// discoverable without reading the source.
void agiDX9LoadShaderConfig();

---
title: Open1560
---

# Installation

1. Download the [**Latest Version**](https://github.com/0x1F9F1/Open1560/releases/download/build/Open1560.zip) and extract the files directly into your MM1 directory. \
   For a fresh installation, only audio.ar, core.ar, and ui.ar are required from the original game.
2. Read the [**README**](./setup.md)
3. To play, run **Open1560.exe**

# FAQ

* Do I need to install the XP patch?
    * No, Open1560 uses a separate executable.
* Do I need to install dgVoodoo2?
    * No, Open1560 uses an OpenGL renderer.
* What does Open1560 change?
    * See a list of changes [here](./changes.md).
* How do I work with the Open1560 code?
    * See the build instructions [here](./building.md).
* How does the DirectX 9 renderer work, and how do I build it quickly?
    * See the [renderer handoff](./handoff_dx9_renderer.md) — project structure, the assembly/C++
      seam, fast build workflow, and the roadmap.
* What are the two D3D9 rendering pathways?
    * See the [rendering pathways design](./dx9_rendering_pathways.md) — fixed-function (RTX Remix
      compatible) versus programmable (PBR shaders), and the rationale for keeping both.
* How bright is each kind of light, and what should I tune?
    * See the [light intensity reference](./light_intensities.md) — every value that feeds a light's
      brightness, what it was measured at in-game, and a suggested order for a tuning pass.
* What is planned for the programmable renderer next?
    * See the [future renderer plan](./future_renderer.md) — sun shadows, area lights, environment
      probes, water and road reflections without screen space, procedural rain puddles, and AO.
* What files make up a city, and what is in them?
    * See the [city format reference](./city_format.md) — the cell table, portals, geometry
      containers, bounds, bangers, facades and the AI road network, field by field, with the closed
      formats decoded from the disassembly.

# Support

If you are having problems or wish to report a bug, [join the discord](https://discord.gg/HHZz27sFEH) or [create an issue](https://github.com/0x1F9F1/Open1560/issues/new)

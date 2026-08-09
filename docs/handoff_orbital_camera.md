# Open1560 — orbital camera handoff

Everything needed to implement a GTA IV/V style orbital camera and prove it works. Written for
someone picking this up cold.

No code here on purpose — this is the map, not the territory. Symbol and file names are given so
you can find things; `handoff_dx9_renderer.md` covers the project as a whole and
`dx9_rendering_pathways.md` covers the renderer.

---

## 1. What is being built

A third-person camera that orbits the player's car, driven by mouse movement, toggled on and off
with the **C** key. Mouse X orbits horizontally, mouse Y changes pitch. It replaces the active
camera while enabled and restores the previous one when switched off.

Out of scope for a first version: collision-aware distance pull-in (camera pushing closer when a
wall is behind it), smoothing/inertia, FOV changes, and any persistence of the camera's angles
across a race. Get a camera that orbits and does not crash first; those are refinements.

---

## 2. Why this is not just "write a camera class"

The constraint that shapes everything is the ARTS_IMPORT/ARTS_EXPORT seam described in
`handoff_dx9_renderer.md` §1. The camera system is almost entirely **closed assembly**: the files
under `code/midtown/mmcamcs` are 800–4000 bytes each and are nearly all import declarations. You
are adding a new participant to a system you cannot read.

Three consequences:

* **Class layouts are frozen.** `check_size` assertions enforce them. `CarCamCS` is exactly `0x118`
  bytes and `BaseCamCS` is `0x9C`. You may derive a new class of your own, but you may not add
  members to theirs.
* **Virtual table slots are frozen.** Your subclass inherits a vtable whose slots the assembly
  indexes by original offset. Overriding existing virtuals is fine; adding new ones is only safe
  after every original.
* **The engine reflects on these objects.** Every camera class implements `GetClass()` returning a
  `MetaClass*`, and there are `DeclareFields()` statics throughout. Something in the engine uses
  that machinery. How it will react to a class it has never heard of is the main unknown — see §4.

---

## 3. The design that has been settled

Derive a new camera from `CarCamCS` and override its `Update()` to compute the camera transform
each frame.

Why that base: `CarCamCS` already carries a `mmCar*` member (the thing to orbit around), and
`mmViewCS::SetCurrentCam()` — which is importable and is how you install a camera — takes a
`CarCamCS*`. `PolarCamCS` derives from the same place and is the closest existing analogue; it is
already a polar/orbital camera, but its `Update()` is closed assembly so it cannot be reused or
parameterised. It is worth reading its header anyway as a shape reference, including the way it
appends its own storage after the base.

The relevant inherited state lives in `BaseCamCS` (see `basecamcs.h`) and is `protected`, so a
subclass can write it directly: a camera matrix, a second matrix, a view pointer, and FOV/near/far
floats. `Matrix34` in this engine is four `Vector3` rows — three axis vectors and a translation —
which is the convention the rest of the renderer uses (`agiMeshSet::M`, the world matrices passed
to `MeshWorld`). Build the orbit position and orientation into that.

Keep your own yaw/pitch/distance as file-scope state in the new translation unit rather than as
members, unless you are confident about the size question in §4.

### Input

* **Key.** Keyboard state is reimplemented C++ — `code/midtown/eventq` / `event.cpp` maintains a
  256-entry key state array with a previous-frame copy alongside it, which is exactly what an
  edge-triggered toggle needs (react to the transition, not the held key). `sdlevent.cpp` maps SDL
  scancodes to the virtual keys that array is indexed by.
* **Check C is free first.** MM1 binds a lot of keys and the original game already has camera
  controls. Confirm nothing else consumes C before taking it, and if it is taken, pick another key
  and say so rather than silently stealing it.
* **Mouse.** SDL3 relative mouse mode, owned by the `agisdl` layer. Relative mode must be enabled
  only while the orbital camera is active and released when it is not, or the mouse is captured in
  menus. Be careful that this does not fight the camera proxy's ImGui overlay, which has its own
  input-capture setting (`DisableGameInputWhileMenuOpen` in `camera_proxy.ini`).

### Installation and restore

`mmViewCS::SetCurrentCam()` installs a camera; `mmViewCS::Instance()` gets the manager. On toggle
off, put back whatever was current before — capture it on toggle on rather than assuming a default.
`MakeActive()` and `Reset()` are virtuals on the base worth implementing sensibly.

---

## 4. The two unknowns to resolve before writing anything

Both are quick to answer by reading, and both silently produce a broken camera if guessed.

1. **How to get the car's world transform.** `mmCar::GetICS()` is inline and returns an
   `asInertialCS*` from the car's `Sim` member. What has *not* been confirmed is how the world
   matrix or position comes out of `asInertialCS`. Find that before writing the orbit maths — a
   camera that orbits the origin instead of the car looks identical to a camera that is not being
   updated at all, and you will waste a deploy cycle telling them apart.

2. **`GetClass()` and `MetaClass`.** Decide what your subclass returns. Returning `CarCamCS`'s may
   be fine, or the engine may need a properly registered class. Look at how `DeclareFields()` and
   the MetaClass registrations work for the existing cameras, and check whether `mmViewCS` or the
   save/serialisation path ever calls `GetClass()` on the current camera. This is the single most
   likely cause of a crash on pressing C.

   Related: if the engine ever allocates or copies a camera by its declared size, a subclass larger
   than `0x118` is a hazard. `PolarCamCS` handles this by declaring explicit trailing storage and
   asserting its own total size; mirror that pattern if you add members.

---

## 5. Build and deploy reality — read this before planning your loop

**There is no local toolchain.** The E: drive was formatted and MSVC went with it. The only way to
build is to push to `dx9/master` and let GitHub Actions do it
(`.github/workflows/build.yml`). See the `open1560-build-via-ci` memory and
`handoff_dx9_renderer.md`.

Two gates run before the build and both must pass, so run them locally before every push — they
take seconds and a failed gate costs you a full CI cycle:

* `python3 tools/format.py --dry` — uses the pinned in-repo `clang-format.exe`, so a local pass
  guarantees a CI pass. Run without `--dry` to fix.
* `python3 tools/asm.py` — note it **rewrites `code/midtown/game.asm`** as a side effect. It finds
  imports and exports by matching a `// <mangled name>` comment on the line *immediately* above
  `ARTS_IMPORT`/`ARTS_EXPORT`. Never write prose between the two; put prose above the mangled name.

**You cannot download the CI artifact yourself.** There is no `gh` CLI, the repo is private, and
reading the stored git credentials is blocked. The user downloads the artifact and drops it into
`D:\MM1\MM1`. Every test cycle therefore costs one round trip through them — which is the reason to
resolve §4 by reading rather than by pushing a guess.

Confirm which build is actually deployed by launching and reading the `Build:` line at the top of
`Open1560.log`. It carries the CI run number and the full commit SHA. File timestamps are the CI
runner's and will mislead you.

Other codebase pitfalls that have each cost real time:

* `mem::cmd_param` **must be declared at namespace scope.** A function-local one constructs after
  argument parsing has already run and silently never receives its value. This has bitten twice.
* `/W4 /WX`. Unused locals are fatal. A `static` function with no callers is C4505 and also fatal —
  annotate deliberately-unreferenced ones rather than suppressing the warning globally.
* Touching a header triggers a near-full rebuild. Irrelevant for CI cost, but batch header edits
  anyway.
* New source files must be added to the component's `premake5.lua` explicitly. `mmcamcs` lists its
  files by name; it does not glob.

---

## 6. How to test

A working loop was established in the previous session. Reuse it rather than rediscovering it.

### Driving the game from the shell

* **Launch detached.** Starting the game as a direct child of the tool shell means it dies when the
  shell exits. Launch it through a detaching wrapper so it survives between tool calls.
* Useful arguments: windowed mode plus the D3D9 renderer selector. The shortcut in the game folder
  shows the normal pair. Add `-ghash` / `-ghashcolor` if you want the geometry diagnostics on.
* **Screenshots.** Get the window handle from the process, take the *client* rectangle, convert to
  screen coordinates, and copy from the screen into a bitmap saved as PNG. Then read the PNG back —
  you can see it. This is how you verify a camera actually moved, and there is no substitute.
* **The window changes size.** It is 640×480 at the menu and 1920×1080 once a race loads. Re-query
  the client rect every time; do not cache it.
* **Reaching gameplay.** From the main menu, "Quick Race" sits at roughly client (95, 202). That
  leads to the vehicle screen, where "Go Drive!" is at roughly client (568, 425). The city takes
  30–45 seconds to load. Move the cursor, pause briefly, then click — clicking without a hover
  frame first has missed its target.

### What to actually verify

1. **It does not crash on C.** Given §4.2 this is a real outcome, not a formality.
2. **The camera orbits the car, not a fixed point.** Drive forward, then orbit. A camera that
   orbits the world origin is the classic symptom of getting the car transform wrong.
3. **Mouse feel.** Both axes, correct directions, no runaway when the mouse leaves the window, no
   pitch flipping over the top or under the ground — clamp pitch.
4. **Toggle restores cleanly.** Press C twice and confirm the original camera comes back and the
   mouse is released.
5. **It survives a race restart and a quit to menu.** The pipeline is torn down and rebuilt between
   menu and race; anything holding stale pointers across that shows up here.
6. **Nothing else regressed.** Check the `DX9 CENSUS` line still shows a high "world share (3D
   only)" — it runs around 90–95% in gameplay. A camera bug that breaks culling or the view matrix
   will move that number.

### Do not break the Remix work

The projection behaviour in `agiDX9Rasterizer::RestoreStateAfterWorldDraw` is load-bearing and was
validated empirically. If you touch view or projection state anywhere, re-check it.

The camera proxy at `D:\last camera proxy version` is the tool that validates it, and a copy is
already in the game folder renamed to `d3d9.dll.disabled` — rename it back to `d3d9.dll` to enable,
and put it back when finished so a normal launch is unaffected. Its `camera_proxy.ini` is already
configured to chain to the system d3d9 rather than a Remix runtime, which is required because there
is no RTX hardware on this machine. Press **F10** in game for its overlay; the Camera tab shows the
live World/View/Projection.

The expected reading, and the regression test: **World and View are identity, Projection is a real
perspective matrix with a zero in the bottom-right.** Identity projection is the bug that stops RTX
Remix path tracing anything after the frame's first blob shadow.

### Housekeeping

* `Open1560.log` is overwritten on every launch. If a run produced something worth keeping, rename
  it before relaunching — do not delete it, and do not assume you can reread it later. This was
  learned by losing one.
* `camera_proxy.log` is held open exclusively while the game runs. You cannot read it until the
  process exits, not even with shared-read.
* Close the game between runs and confirm the process is gone before relaunching.

---

## 7. Definition of done

C toggles an orbital camera that follows the player's car, is driven by the mouse on both axes with
pitch clamped, restores the previous camera and releases the mouse when toggled off, survives a quit
to menu, and leaves the census and the proxy's projection reading unchanged. Verified by
screenshots showing the camera at several orbit positions around the car, not by reasoning.

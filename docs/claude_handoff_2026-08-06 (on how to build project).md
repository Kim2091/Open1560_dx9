## How to build Open1560 FAST and CLEANLY

### The build command

```bash
MSYS_NO_PATHCONV=1 cmd.exe /c "D:\Open1560\build\tmp\dobuild.bat" > /d/Open1560/build/tmp/out.log 2>&1
grep -E "MSBUILD_EXIT|: error|Error\(s\)" /d/Open1560/build/tmp/out.log | tail -5
```

Success = `MSBUILD_EXIT=0` and `0 Error(s)`. The toolchain is not on `PATH` and cannot be
auto-detected; `dobuild.bat` sets `PATH`/`INCLUDE`/`LIB`/`TEMP` explicitly and is self-contained.
On success it auto-deploys to `E:\MM1`.

### Rules that actually save time

1. **KILL THE GAME BEFORE BUILDING. (if the game is running actually)** A running `Open1560.exe` holds `E:\MM1\Open1560.exe` and the
   post-build `xcopy` fails with **`error MSB3073 ... exited with code 4`** — after a *successful*
   compile. Cost us a full rebuild cycle.
   ```bash
   MSYS_NO_PATHCONV=1 cmd.exe /c "taskkill /IM Open1560.exe /F"
   ```
   Note `nav4.ps1` kills the game at the *start* of a run, not the end — so one is usually still
   running when you go to build.

2. **Always build in the background** (`run_in_background: true`) and wait on the notification.
   A full rebuild is **~7 minutes**. Do not poll in a loop; it wastes the whole wait.

3. **Touching a header = full rebuild.** `agi/rsys.h`, `agiworld/meshset.h`, `agidx9/dx9rsys.h`,
   `agidx9/dx9pipe.h` invalidate the PCH and recompile nearly everything (~7 min). Editing only
   `.cpp` files is much faster. **Batch header changes together** — several separate header edits
   cost several full rebuilds.

4. **`/W4 /WX` — warnings are errors.** Two that bit us:
   - `std::getenv` → C4996 deprecation. Wrap in
     `#pragma warning(push)` / `disable : 4996` / `pop`.
   - Unused variables after removing a code path.

5. **`C:` is 100% FULL (0 bytes free).** This already caused
   `write error: No space left on device` from shell pipes (`grep`, `awk`). Builds still work
   because `dobuild.bat` redirects `TEMP`/`TMP` to `D:` and all obj/bin output is on `D:`, but
   **shell commands that buffer through C: temp will fail intermittently.** Prefer the PowerShell
   tool for reads/greps until space is freed. Nothing reclaimable was found in temp (1.4 MB) — this
   needs the user to clear real files.

6. **`E:` is an external SATA SSD over USB and drops out under load.** Symptom: `MSBUILD_EXIT=-1073741818`
   (`STATUS_IN_PAGE_ERROR`) with *zero* output, `Test-Path` on toolchain dirs returning False, and
   `disk`/`Ntfs` errors in the System event log. It is **not** a dead disk — it recovers. If a build
   fails this way, just retry; do not go on a hardware investigation like I did.

---

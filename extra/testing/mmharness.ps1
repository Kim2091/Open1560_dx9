# Open1560 test harness - self-contained.
#
# Everything here is Win32 via Add-Type plus GDI+; it depends on nothing outside this repo. Dot-source
# it and call the functions:
#
#   . D:\Open1560\build\tmp\mmharness.ps1
#   Start-MM -Args @('-d3d9','-window')
#   Wait-MMGameplay
#   Save-MMShot "$env:TEMP\shot.png"
#
# Two things this harness gets right that cost real time to discover:
#
#  * Clicks are dropped unless the window is genuinely foreground. Posting WM_LBUTTONDOWN is not
#    enough and neither is SetForegroundWindow on its own from a background process - Windows
#    refuses the focus change unless the calling thread is attached to the target's input queue.
#    Focus-MMWindow does that attach/detach dance, and every input helper calls it first.
#
#  * The game reads input through DirectInput, not the window message queue, so posted key messages
#    are ignored entirely. Keys have to go through keybd_event, which injects at a lower level.

Add-Type -AssemblyName System.Drawing

if (-not ('MMWin32' -as [type])) {
Add-Type @'
using System;
using System.Runtime.InteropServices;

public class MMWin32
{
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);

    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, IntPtr extra);

    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP   = 0x0004;

    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);

    // Process.MainWindowHandle is populated from a cached snapshot and is frequently 0 for a
    // process started moments ago, so the window is found by enumeration instead.
    //
    // IsWindowVisible is deliberately NOT required. The game's window reports itself as not visible
    // in perfectly ordinary states - unfocused, or freshly created - and filtering on it silently
    // finds nothing, which then looks exactly like "the game never started". Size is the reliable
    // discriminator against the IME and CRT helper windows that share the process.
    public static IntPtr FindWindowForPid(uint wanted)
    {
        IntPtr found = IntPtr.Zero;
        int bestArea = 0;

        EnumWindows(delegate(IntPtr hWnd, IntPtr lp)
        {
            uint pid;
            GetWindowThreadProcessId(hWnd, out pid);

            if (pid == wanted)
            {
                RECT r;
                GetClientRect(hWnd, out r);

                int area = (r.Right - r.Left) * (r.Bottom - r.Top);

                if ((r.Right - r.Left) >= 320 && (r.Bottom - r.Top) >= 200 && area > bestArea)
                {
                    bestArea = area;
                    found = hWnd;
                }
            }

            return true;
        }, IntPtr.Zero);

        return found;
    }

    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP   = 0x0202;
    public const uint WM_MOUSEMOVE   = 0x0200;
    public const uint MK_LBUTTON     = 0x0001;

    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);

    public const uint KEYEVENTF_KEYUP = 0x0002;
}
'@
}

function Get-MMProcess {
    return Get-Process Open1560 -ErrorAction SilentlyContinue | Select-Object -First 1
}

function Get-MMWindow {
    $p = Get-MMProcess
    if (-not $p) { return [IntPtr]::Zero }
    return [MMWin32]::FindWindowForPid([uint32]$p.Id)
}

function Stop-MM {
    Get-Process Open1560 -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 2
}

function Start-MM {
    param(
        [string[]] $GameArgs = @('-d3d9', '-window'),
        [string]   $Dir = 'E:\MM1',
        [hashtable] $Env = @{},
        [int]      $WaitSeconds = 34
    )

    Stop-MM

    # Do NOT clear Open1560.log here. On a crash the game moves it into crashes\, and an emptied log
    # archives as an empty file - which is how a real crash report got destroyed once already. The
    # game truncates it on startup anyway, so clearing buys nothing.
    foreach ($k in $Env.Keys) { Set-Item -Path "Env:$k" -Value $Env[$k] }

    Start-Process -FilePath (Join-Path $Dir 'Open1560.exe') -ArgumentList $GameArgs -WorkingDirectory $Dir
    Start-Sleep -Seconds $WaitSeconds

    return (Get-MMProcess) -ne $null
}

function Focus-MMWindow {
    $hwnd = Get-MMWindow
    if ($hwnd -eq [IntPtr]::Zero) { return $false }

    # Attach to the target's input queue, or Windows silently refuses the foreground change.
    $target = [MMWin32]::GetWindowThreadProcessId($hwnd, [IntPtr]::Zero)
    $self   = [MMWin32]::GetCurrentThreadId()

    [void][MMWin32]::AttachThreadInput($self, $target, $true)
    [void][MMWin32]::ShowWindow($hwnd, 5)   # SW_SHOW
    [void][MMWin32]::BringWindowToTop($hwnd)
    [void][MMWin32]::SetForegroundWindow($hwnd)
    [void][MMWin32]::AttachThreadInput($self, $target, $false)

    Start-Sleep -Milliseconds 350
    return ([MMWin32]::GetForegroundWindow() -eq $hwnd)
}

function Get-MMClientRect {
    $hwnd = Get-MMWindow
    if ($hwnd -eq [IntPtr]::Zero) { return $null }

    $r = New-Object MMWin32+RECT
    if (-not [MMWin32]::GetClientRect($hwnd, [ref]$r)) { return $null }

    $origin = New-Object MMWin32+POINT
    $origin.X = 0; $origin.Y = 0
    [void][MMWin32]::ClientToScreen($hwnd, [ref]$origin)

    return [pscustomobject]@{
        Width   = $r.Right - $r.Left
        Height  = $r.Bottom - $r.Top
        ScreenX = $origin.X
        ScreenY = $origin.Y
    }
}

# Click at client coordinates.
#
# Uses the real pointer (SetCursorPos + mouse_event) rather than PostMessage, which was tried first
# and does NOT work: SDL re-maps posted mouse coordinates, so a posted click lands somewhere other
# than where it was aimed. That is worse than not working, because the click still hits *something* -
# a test aimed at "Quit to Race Menu" landed on "Help" and then reported a pass for a transition it
# had never performed.
#
# Because this moves the real pointer, it is guarded: the game window must be confirmed foreground
# immediately beforehand, or the click is refused rather than delivered to whatever else is on
# screen. The previous cursor position is restored afterwards.
function Send-MMClick {
    param([int]$X, [int]$Y, [int]$Repeat = 2)

    if (-not (Focus-MMWindow)) {
        Write-Host "  (click refused: game window is not foreground)"
        return $false
    }

    $c = Get-MMClientRect
    if (-not $c) { return $false }

    $saved = New-Object MMWin32+POINT
    [void][MMWin32]::GetCursorPos([ref]$saved)

    for ($i = 0; $i -lt $Repeat; $i++) {
        # Re-check every iteration: focus can be lost between clicks.
        if (([MMWin32]::GetForegroundWindow() -ne (Get-MMWindow))) { break }

        [void][MMWin32]::SetCursorPos($c.ScreenX + $X, $c.ScreenY + $Y)
        Start-Sleep -Milliseconds 200
        [MMWin32]::mouse_event([MMWin32]::MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 70
        [MMWin32]::mouse_event([MMWin32]::MOUSEEVENTF_LEFTUP, 0, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 450
    }

    [void][MMWin32]::SetCursorPos($saved.X, $saved.Y)

    return $true
}

function Send-MMKey {
    param([byte]$Vk, [int]$HoldMs = 80)

    if (-not (Focus-MMWindow)) { return $false }

    [MMWin32]::keybd_event($Vk, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds $HoldMs
    [MMWin32]::keybd_event($Vk, 0, [MMWin32]::KEYEVENTF_KEYUP, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 250

    return $true
}

$MMKey = @{ Escape = 0x1B; Enter = 0x0D; Space = 0x20; Up = 0x26; Down = 0x28; Left = 0x25; Right = 0x27 }

# Screen-grab the window's client area. CopyFromScreen rather than PrintWindow/BitBlt on the window
# DC: a Direct3D swapchain does not render into the GDI device context, so those come back black.
function Save-MMShot {
    param([string]$Path)

    if (-not (Focus-MMWindow)) { return $false }
    Start-Sleep -Milliseconds 250

    $c = Get-MMClientRect
    if (-not $c -or $c.Width -le 0) { return $false }

    $bmp = New-Object System.Drawing.Bitmap($c.Width, $c.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($c.ScreenX, $c.ScreenY, 0, 0, (New-Object System.Drawing.Size($c.Width, $c.Height)))
    $g.Dispose()

    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }

    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()

    Write-Host "shot: $Path ($($c.Width)x$($c.Height))"
    return $true
}

# "Are we in the city yet?"
#
# NOT client width. That was the first attempt and it is wrong: gameplay only runs wider than the
# 640x480 menus if the player's profile asks for a wider resolution, and a profile set to 640x480
# makes the test unpassable - which looks exactly like the menu navigation failing, and cost a
# debugging session to work out.
#
# The census is the honest signal. It reports world-space triangles submitted per frame, which is
# zero on every menu and immediately non-zero once a city is drawing.
function Wait-MMGameplay {
    param([int]$TimeoutSeconds = 90, [string]$LogPath = 'E:\MM1\Open1560.log')

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)

    while ((Get-Date) -lt $deadline) {
        if (-not (Get-MMProcess)) { return $false }

        $last = Get-MMLogLines 'DX9 CENSUS' -LogPath $LogPath -Last 1

        if ($last -and ($last -match 'world=(\d+) tris') -and ([int]$Matches[1] -gt 200)) {
            return $true
        }

        Start-Sleep -Seconds 2
    }

    return $false
}

function Test-MMAlive {
    return (Get-MMProcess) -ne $null
}

# A crash leaves an ACCESS_VIOLATION line in the log and pops a modal dialog, so "the process still
# exists" is not the same as "it did not crash". Check both.
function Get-MMCrash {
    param([string]$LogPath = 'E:\MM1\Open1560.log')

    if (-not (Test-Path $LogPath)) { return $null }

    $hits = Select-String -Path $LogPath -Pattern 'ACCESS_VIOLATION|ABORT:' -ErrorAction SilentlyContinue
    if (-not $hits) { return $null }

    return ($hits | ForEach-Object { $_.Line.Trim() }) -join ' | '
}

function Get-MMLogLines {
    param([string]$Pattern, [string]$LogPath = 'E:\MM1\Open1560.log', [int]$Last = 6)

    if (-not (Test-Path $LogPath)) { return @() }
    return (Select-String -Path $LogPath -Pattern $Pattern -ErrorAction SilentlyContinue |
            Select-Object -Last $Last | ForEach-Object { $_.Line.Trim() })
}

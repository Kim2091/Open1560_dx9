# Open1560 regression pass. Self-contained: uses only build/tmp/mmharness.ps1.
#
#   powershell -File D:\Open1560\build\tmp\mmtest.ps1 [-OutDir <dir>] [-Mask <hex>]
#
# Covers the paths that have actually broken in this backend, in the order they broke:
#   1. reach gameplay at all
#   2. survive the menu -> gameplay device reset (the RTX Remix bridge case)
#   3. survive Quit to Race Menu (the BuildLightPool dangling-registry case)
#   4. render vehicle reflections without crashing (the agiRQ.SphMap case)

param(
    [string] $OutDir = "$env:TEMP\mmtest",
    [string] $Mask = $null,
    [string[]] $GameArgs = @('-d3d9', '-window')
)

. "$PSScriptRoot\mmharness.ps1"

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }

$results = [System.Collections.Generic.List[object]]::new()

function Add-Result {
    param([string]$Name, [bool]$Pass, [string]$Detail = '')
    $results.Add([pscustomobject]@{ Test = $Name; Result = $(if ($Pass) { 'PASS' } else { 'FAIL' }); Detail = $Detail })
    Write-Host ("[{0}] {1} {2}" -f $(if ($Pass) { 'PASS' } else { 'FAIL' }), $Name, $Detail)
}

$envVars = @{}
if ($Mask) { $envVars['OPEN1560_NATIVE_MASK'] = $Mask }

Write-Host "=== Open1560 regression pass ==="
Write-Host "args: $($GameArgs -join ' ')  mask: $(if ($Mask) { $Mask } else { '(default)' })"

# --- 1. launch -------------------------------------------------------------------------------
if (-not (Start-MM -GameArgs $GameArgs -Env $envVars)) {
    Add-Result 'launch' $false 'process died before the menu'
    $results | Format-Table -AutoSize
    exit 1
}
Add-Result 'launch' $true

Save-MMShot (Join-Path $OutDir '01-menu.png') | Out-Null

# --- 2. menu -> gameplay ---------------------------------------------------------------------
# Two steps, not one: Quick Race at (95,203) leads to the VEHICLE SELECT screen, and "Go Drive!"
# at (566,405) is what actually loads the city. Both are on the 640x480 menu. Alternating the two
# and watching client width is robust to whichever screen the game happens to start on, and to a
# click being swallowed while a menu is still settling.
$entered = $false
for ($i = 1; $i -le 6 -and -not $entered; $i++) {
    if ($i % 2 -eq 1) { Send-MMClick 95 203 | Out-Null } else { Send-MMClick 566 405 | Out-Null }
    $entered = Wait-MMGameplay -TimeoutSeconds 20
}

Add-Result 'reached gameplay' $entered
if (-not $entered) {
    Add-Result 'crash check' ((Get-MMCrash) -eq $null) (Get-MMCrash)
    $results | Format-Table -AutoSize
    Stop-MM
    exit 1
}

Start-Sleep -Seconds 10
Save-MMShot (Join-Path $OutDir '02-gameplay.png') | Out-Null

$res = Get-MMClientRect
Add-Result 'device survived resolution change' ($res.Width -gt 640) "client $($res.Width)x$($res.Height)"
Add-Result 'no crash entering gameplay' ((Get-MMCrash) -eq $null) (Get-MMCrash)

# --- 3. camera sweep, to put a vehicle and the environment on screen --------------------------
for ($i = 0; $i -lt 3; $i++) {
    Send-MMClick 427 240 -Repeat 1 | Out-Null
    Start-Sleep -Seconds 3
    Save-MMShot (Join-Path $OutDir ("03-camera-{0}.png" -f $i)) | Out-Null
}
Add-Result 'survived camera changes' (Test-MMAlive)

# --- 4. pause menu -> Quit to Race Menu -------------------------------------------------------
Send-MMKey $MMKey.Escape | Out-Null
Start-Sleep -Seconds 3
Save-MMShot (Join-Path $OutDir '04-pause.png') | Out-Null

# "Quit to Race Menu" is the fourth item, at roughly client (428,287) at 854x480.
$qx = [int]($res.Width / 2)
$qy = [int]($res.Height * 0.60)
Send-MMClick $qx $qy | Out-Null
Start-Sleep -Seconds 20

Save-MMShot (Join-Path $OutDir '05-after-quit.png') | Out-Null

$crash = Get-MMCrash
Add-Result 'no crash on quit to race menu' ($crash -eq $null) $crash

# --- 5. report --------------------------------------------------------------------------------
Write-Host ''
Write-Host '--- log highlights ---'
Get-MMLogLines 'DX9 probe|Pathway B: programmable|NATIVE_MASK|DX9 RT' -Last 8 | ForEach-Object { Write-Host "  $_" }
Write-Host ''
Get-MMLogLines 'DX9 CENSUS' -Last 2 | ForEach-Object { Write-Host "  $_" }

Stop-MM

Write-Host ''
$results | Format-Table -AutoSize

$failed = @($results | Where-Object { $_.Result -eq 'FAIL' }).Count
Write-Host "shots in $OutDir"
exit $(if ($failed -gt 0) { 1 } else { 0 })

# Frame-rate comparison across renderer configurations.
#
# The census prints one line every 120 frames, so counting lines over a known wall time gives an
# average frame rate without needing an in-game counter. Crude, but it is a real measurement of the
# actual game loop rather than a synthetic one, and the differences being chased here are large.

param([int]$Seconds = 45)

. "$PSScriptRoot\mmharness.ps1"

function Measure-Config {
    param([string]$Name, [string[]]$GameArgs, [hashtable]$Env = @{})

    Start-MM -GameArgs $GameArgs -Env $Env | Out-Null

    $in = $false
    for ($i = 1; $i -le 8 -and -not $in; $i++) {
        if ($i % 2 -eq 1) { Send-MMClick 95 203 | Out-Null } else { Send-MMClick 566 405 | Out-Null }
        $in = Wait-MMGameplay -TimeoutSeconds 20
    }

    if (-not $in) { Stop-MM; return [pscustomobject]@{ Config = $Name; FPS = 'n/a'; Note = 'never reached gameplay' } }

    # Settle, then count census lines over the sample window.
    Start-Sleep -Seconds 5
    $before = (Get-MMLogLines 'DX9 CENSUS' -Last 100000).Count
    Start-Sleep -Seconds $Seconds
    $after = (Get-MMLogLines 'DX9 CENSUS' -Last 100000).Count

    $frames = ($after - $before) * 120
    $fps = [math]::Round($frames / $Seconds, 1)

    $last = Get-MMLogLines 'DX9 CENSUS' -Last 1
    $tris = if ($last -match 'world=(\d+) tris in (\d+) calls') { "$($Matches[1]) tris / $($Matches[2]) calls" } else { '' }

    Stop-MM

    return [pscustomobject]@{ Config = $Name; FPS = $fps; Note = $tris }
}

$results = @()
$results += Measure-Config 'Pathway B (shaders, default)' @('-d3d9','-window')
$results += Measure-Config 'Pathway A (fixed function)'   @('-d3d9','-window','-d3d9shaders','0')
$results += Measure-Config 'Pathway A, CPU transform'     @('-d3d9','-window','-d3d9shaders','0') @{ OPEN1560_NATIVE_MASK = '0' }
$results += Measure-Config 'Pathway B, no smooth normals' @('-d3d9','-window','-smoothnormals','0')

$results | Format-Table -AutoSize

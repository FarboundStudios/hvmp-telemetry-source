[CmdletBinding()]
param([switch]$Tests)

$ErrorActionPreference = 'Stop'
$sourceRoot = $PSScriptRoot
$buildDirectory = Join-Path $sourceRoot 'build'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Install Visual Studio Build Tools with Desktop development with C++ and the Windows SDK.'
}
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$visualStudio) {
    throw 'Visual Studio C++ build tools were not found.'
}
Import-Module (Join-Path $visualStudio 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $visualStudio -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

function Invoke-BuildTool {
    param([string]$Tool, [string[]]$ToolArguments)
    & $Tool @ToolArguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Tool failed with exit code $LASTEXITCODE."
    }
}

New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
Push-Location $buildDirectory
try {
    $native = Join-Path $sourceRoot 'native'
    $minhook = Join-Path $native 'vendor\minhook\src'
    $commonFlags = @(
        '/nologo', '/std:c++17', '/EHsc', '/W4', '/utf-8', '/O2', '/MT',
        "/I$native", "/I$native\sdk\include"
    )
    $minhookSources = @(
        "$minhook\buffer.c", "$minhook\hook.c", "$minhook\trampoline.c", "$minhook\hde\hde64.c"
    )
    Invoke-BuildTool 'cl.exe' (@('/nologo', '/O2', '/MT', '/c') + $minhookSources)
    Invoke-BuildTool 'ml64.exe' @('/nologo', '/c', '/Foheavymp-clock-abi.obj', "$native\heavymp_clock_abi.asm")

    $sources = @(
        "$native\heavymp_telemetry.cpp",
        "$native\heavymp_content.cpp",
        "$native\heavymp_vehicles.cpp",
        "$native\heavymp_native.cpp"
    )
    $hookObjects = @('buffer.obj', 'hook.obj', 'trampoline.obj', 'hde64.obj')
    $libraries = @('ws2_32.lib', 'user32.lib', 'gdi32.lib', 'd3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    $dllArguments = $commonFlags + @('/LD') + $sources + $hookObjects + @(
        'heavymp-clock-abi.obj', '/link'
    ) + $libraries + @(
        "/DEF:$native\heavymp_telemetry.def",
        '/IMPLIB:heavymp-telemetry.lib', '/OUT:heavymp-telemetry.dll'
    )
    Invoke-BuildTool 'cl.exe' $dllArguments

    if ($Tests) {
        $testNames = @(
            'content_reader', 'hud_notification', 'dispatch_telemetry', 'collision_policy',
            'traffic_capture', 'traffic_lab_wire', 'traffic_lease', 'traffic_motion',
            'snapshot_pool', 'packet_inbox', 'performance_counter', 'work_budget',
            'car_engine_state', 'siren_latch', 'vehicle_audio', 'zone_policy', 'player_projection'
        )
        foreach ($name in $testNames) {
            $executable = "$name-test.exe"
            $testSource = Join-Path $native ($name + '_test.cpp')
            $testArguments = $commonFlags + @($testSource) + $hookObjects + @(
                "/Fe:$executable", '/link'
            ) + $libraries
            Invoke-BuildTool 'cl.exe' $testArguments
            Invoke-BuildTool (Join-Path $buildDirectory $executable) @()
        }
        Write-Host "$($testNames.Count) native checks passed."
    }
    Write-Host "Built $(Join-Path $buildDirectory 'heavymp-telemetry.dll')"
} finally {
    Pop-Location
}


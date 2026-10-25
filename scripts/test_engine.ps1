[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$sourceRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Build Tools with the C++ compiler are required; vswhere.exe was not found.'
}
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw 'No Visual Studio installation with the x64 C++ compiler was found.'
}
$vcvars64 = Join-Path ($installation | Select-Object -First 1) 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars64)) {
    throw "Compiler environment script not found: $vcvars64"
}

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$buildName = 'hid-engine-tests-' + [guid]::NewGuid().ToString('N')
$buildDirectory = [IO.Path]::GetFullPath((Join-Path $tempRoot $buildName))
$null = New-Item -ItemType Directory -Path $buildDirectory
$previousEnvironment = @{}

try {
    # Import Microsoft's own compiler setup so MSVC can locate its C runtime and SDK.
    $setupFile = Join-Path $buildDirectory 'compiler-environment.cmd'
    @('@echo off', ('call "{0}" >nul' -f $vcvars64), 'if errorlevel 1 exit /b 1', 'set') |
        Set-Content -LiteralPath $setupFile -Encoding Ascii
    $compilerEnvironment = & $env:ComSpec /d /c $setupFile
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the Visual Studio compiler environment.' }
    foreach ($entry in $compilerEnvironment) {
        if ($entry -match '^([^=]+)=(.*)$') {
            $variableName = $Matches[1]
            $previousEnvironment[$variableName] = [Environment]::GetEnvironmentVariable($variableName, 'Process')
            [Environment]::SetEnvironmentVariable($variableName, $Matches[2], 'Process')
        }
    }
    $compiler = (Get-Command cl.exe -CommandType Application -ErrorAction Stop).Source
    $executable = Join-Path $buildDirectory 'test_engine.exe'
    Push-Location -LiteralPath $buildDirectory
    try {
        & $compiler /nologo /TC /std:c11 /W4 /WX /O2 /D_CRT_SECURE_NO_WARNINGS `
            "/I$sourceRoot" (Join-Path $sourceRoot 'tests\test_engine.c') `
            (Join-Path $sourceRoot 'hid_engine.c') "/Fe:$executable"
        if ($LASTEXITCODE -ne 0) { throw "Host C compilation failed (exit $LASTEXITCODE)." }
        & $executable
        if ($LASTEXITCODE -ne 0) { throw "HID engine tests failed (exit $LASTEXITCODE)." }
        $wifiExecutable = Join-Path $buildDirectory 'test_wifi_watch.exe'
        & $compiler /nologo /TC /std:c11 /W4 /WX /O2 `
            "/I$sourceRoot" (Join-Path $sourceRoot 'tests\test_wifi_watch.c') `
            (Join-Path $sourceRoot 'wifi_watch.c') "/Fe:$wifiExecutable"
        if ($LASTEXITCODE -ne 0) { throw "Wi-Fi watch compilation failed (exit $LASTEXITCODE)." }
        & $wifiExecutable
        if ($LASTEXITCODE -ne 0) { throw "Wi-Fi watch tests failed (exit $LASTEXITCODE)." }
        $bridgeExecutable = Join-Path $buildDirectory 'test_hid_bridge.exe'
        & $compiler /nologo /TC /std:c11 /W4 /WX /O2 `
            "/I$(Join-Path $sourceRoot 'tests\stubs')" "/I$sourceRoot" `
            (Join-Path $sourceRoot 'tests\test_hid_bridge.c') `
            (Join-Path $sourceRoot 'hid_bridge.c') `
            (Join-Path $sourceRoot 'hid_engine.c') "/Fe:$bridgeExecutable"
        if ($LASTEXITCODE -ne 0) { throw "HID bridge compilation failed (exit $LASTEXITCODE)." }
        & $bridgeExecutable
        if ($LASTEXITCODE -ne 0) { throw "HID bridge tests failed (exit $LASTEXITCODE)." }
    }
    finally {
        Pop-Location
    }
}
finally {
    foreach ($variableName in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($variableName, $previousEnvironment[$variableName], 'Process')
    }
    # Only remove the unique directory created by this invocation, after checking its boundary.
    $resolvedBuild = [IO.Path]::GetFullPath($buildDirectory)
    $tempBoundary = $tempRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if ($resolvedBuild.StartsWith($tempBoundary, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolvedBuild) -eq $buildName -and
        $buildName -match '^hid-engine-tests-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
    }
}

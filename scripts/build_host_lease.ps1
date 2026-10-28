[CmdletBinding()]
param([string]$OutputDirectory = 'build-host-tests')
$ErrorActionPreference = 'Stop'
$sourceRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$buildPath = [IO.Path]::GetFullPath((Join-Path $sourceRoot $OutputDirectory))
if (-not $buildPath.StartsWith($sourceRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Host test output must remain inside the firmware project.'
}
$null = New-Item -ItemType Directory -Path $buildPath -Force
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ Build Tools are required.' }
$vcvars64 = Join-Path ($installation | Select-Object -First 1) 'VC\Auxiliary\Build\vcvars64.bat'
$setupFile = Join-Path $buildPath 'compiler-environment.cmd'
@('@echo off', ('call "{0}" >nul' -f $vcvars64), 'if errorlevel 1 exit /b 1', 'set') |
    Set-Content -LiteralPath $setupFile -Encoding Ascii
$previousEnvironment = @{}
try {
    $compilerEnvironment = & $env:ComSpec /d /c $setupFile
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize MSVC.' }
    foreach ($entry in $compilerEnvironment) {
        if ($entry -match '^([^=]+)=(.*)$') {
            $variableName = $Matches[1]
            $previousEnvironment[$variableName] = [Environment]::GetEnvironmentVariable($variableName, 'Process')
            [Environment]::SetEnvironmentVariable($variableName, $Matches[2], 'Process')
        }
    }
    $compiler = (Get-Command cl.exe -CommandType Application -ErrorAction Stop).Source
    $library = Join-Path $buildPath 'hid_lease_host.dll'
    Push-Location -LiteralPath $buildPath
    try {
        & $compiler /nologo /TC /std:c11 /W4 /WX /O2 /LD `
            "/I$(Join-Path $sourceRoot 'tests\stubs')" "/I$sourceRoot" `
            (Join-Path $sourceRoot 'tests\lease_host.c') `
            (Join-Path $sourceRoot 'hid_bridge.c') `
            (Join-Path $sourceRoot 'hid_lease.c') `
            (Join-Path $sourceRoot 'hid_engine.c') "/Fe:$library"
        if ($LASTEXITCODE -ne 0) { throw 'Production lease host DLL build failed.' }
        Write-Output "Host test DLL: $library (no network or USB input)."
    } finally { Pop-Location }
} finally {
    foreach ($variableName in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($variableName, $previousEnvironment[$variableName], 'Process')
    }
}

[CmdletBinding()]
param(
    [switch]$CompileOnly,
    [string]$PythonExecutable = '',
    [ValidateRange(1,64)][int]$Jobs = 8
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$toolRoot = Join-Path $projectRoot '.tools'
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'toolchain.lock.json') -Raw | ConvertFrom-Json
$wifiConfigPath = Join-Path $projectRoot 'wifi_config.h'
if (-not $CompileOnly) {
    if (-not (Test-Path -LiteralPath $wifiConfigPath)) {
        throw 'Copy wifi_config.h.example to wifi_config.h and fill Wi-Fi settings, or use -CompileOnly (not deployable).'
    }
    $wifiConfigText = [IO.File]::ReadAllText($wifiConfigPath)
    foreach ($definitionName in @('WIFI_SSID', 'WIFI_PASSWORD')) {
        $pattern = '(?m)^\s*#define\s+' + [regex]::Escape($definitionName) + '\s+"((?:\\.|[^"\\])*)"\s*(?://[^\r\n]*)?\r?$'
        $definition = [regex]::Match($wifiConfigText, $pattern)
        if (-not $definition.Success -or [string]::IsNullOrWhiteSpace($definition.Groups[1].Value) -or
            $definition.Groups[1].Value -match '^CHANGE_ME') {
            throw "Set $definitionName in local wifi_config.h before building for deployment."
        }
    }
    $wifiConfigText = $null
    $definition = $null
}
$executables = @{}
foreach ($archive in $lock.archives) {
    $executable = Join-Path $toolRoot $archive.executable
    $marker = Join-Path (Join-Path $toolRoot $archive.directory) '.archive-sha256'
    if (-not (Test-Path -LiteralPath $executable) -or -not (Test-Path -LiteralPath $marker) -or
        [IO.File]::ReadAllText($marker).Trim() -ne $archive.sha256) {
        throw "Missing/mismatched $($archive.name). Run scripts/setup-toolchain.ps1 first."
    }
    $executables[$archive.name] = $executable
}
$sdk = Join-Path $toolRoot 'pico-sdk'
$sdkHead = & git -C $sdk rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $sdkHead.Trim() -ne $lock.sdk.commit) { throw 'Pico SDK revision differs from the lock file.' }
if (-not $PythonExecutable) {
    $pythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
    if (-not $pythonCommand) { throw 'Python 3 is required. Pass -PythonExecutable with its full path.' }
    $PythonExecutable = $pythonCommand.Source
}
& $PythonExecutable -c 'import sys; assert sys.version_info >= (3, 9), "Python 3.9+ is required"'
if ($LASTEXITCODE -ne 0) { throw 'Python check failed.' }
$cmake = $executables['cmake']
$ninja = $executables['ninja']
$compiler = $executables['arm-gcc']
$compilerBin = Split-Path -Parent $compiler
$toolchain = Split-Path -Parent $compilerBin
$mode = if ($CompileOnly) { 'ON' } else { 'OFF' }
$folder = if ($CompileOnly) { 'build-compile-only' } else { 'build-verified' }
$buildPath = Join-Path $projectRoot $folder
$previousSdk = $env:PICO_SDK_PATH
$previousToolchain = $env:PICO_TOOLCHAIN_PATH
$previousPath = $env:PATH
try {
    $env:PICO_SDK_PATH = $sdk
    $env:PICO_TOOLCHAIN_PATH = $toolchain
    $env:PATH = $compilerBin + ';' + (Split-Path -Parent $cmake) + ';' + (Split-Path -Parent $ninja) + ';' + $previousPath
    & $cmake -S $projectRoot -B $buildPath -G Ninja '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_MAKE_PROGRAM=$ninja" "-DPython3_EXECUTABLE=$PythonExecutable" '-DPICO_BOARD=pico2_w' "-DPICO_SDK_PATH=$sdk" "-DPICO_TOOLCHAIN_PATH=$toolchain" "-Dpioasm_DIR=$(Split-Path -Parent $executables['pioasm'])" "-Dpicotool_DIR=$(Split-Path -Parent $executables['picotool'])" "-DHID_COMPILE_ONLY=$mode"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    & $cmake --build $buildPath --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw "Firmware build failed ($LASTEXITCODE)" }
    $artifact = Join-Path $buildPath 'hid_usb.uf2'
    $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    $versionMatch = [regex]::Match([IO.File]::ReadAllText((Join-Path $projectRoot 'CMakeLists.txt')), 'pico_set_program_version\(hid_usb "([^"\r\n]+)"\)')
    if (-not $versionMatch.Success) { throw 'Firmware version declaration is missing.' }
    $manifest = [ordered]@{
        firmware_version = $versionMatch.Groups[1].Value
        compile_only = [bool]$CompileOnly
        deployable_configuration = -not [bool]$CompileOnly
        board = 'pico2_w'
        sdk_commit = $sdkHead.Trim()
        toolchain_lock_sha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'toolchain.lock.json') -Algorithm SHA256).Hash.ToLowerInvariant()
        python = (& $PythonExecutable --version | Out-String).Trim()
        compiler = (& $compiler --version | Select-Object -First 1)
        artifact = 'hid_usb.uf2'
        artifact_sha256 = $hash
        built_utc = [DateTime]::UtcNow.ToString('o')
    }
    $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $buildPath 'build-manifest.json') -Encoding UTF8
    if ($CompileOnly) {
        Set-Content -LiteralPath (Join-Path $buildPath 'NOT_FOR_DEPLOYMENT.txt') -Value 'COMPILE CHECK ONLY. Wi-Fi credentials are disabled. Do not flash this artifact.' -Encoding ASCII
        Write-Output "COMPILE CHECK PASSED (NOT FOR DEPLOYMENT): $artifact"
    } else {
        Write-Output "Built with local Wi-Fi configuration: $artifact"
    }
    Write-Output "SHA256: $hash"
    Write-Output 'No device was flashed.'
} finally {
    $env:PICO_SDK_PATH = $previousSdk
    $env:PICO_TOOLCHAIN_PATH = $previousToolchain
    $env:PATH = $previousPath
}

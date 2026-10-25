param([string]$BuildDirectory = 'build-verified')

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$sdkRoot = Join-Path $env:USERPROFILE '.pico-sdk'
$cmake = Join-Path $sdkRoot 'cmake\v4.3.4\bin\cmake.exe'
$ninja = Join-Path $sdkRoot 'ninja\v1.13.2\ninja.exe'
$sdk = Join-Path $sdkRoot 'sdk\2.3.1'
$toolchain = Join-Path $sdkRoot 'toolchain\15_2_Rel1'
$python = Join-Path $sdkRoot 'python\3.13.7\python.exe'

foreach ($path in @($cmake, $ninja, $sdk, $toolchain, $python)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Pico SDK/tool not found: $path" }
}
$wifiConfigPath = Join-Path $projectRoot 'wifi_config.h'
if (-not (Test-Path -LiteralPath $wifiConfigPath)) {
    throw 'Copy wifi_config.h.example to wifi_config.h and enter Wi-Fi settings before building for deployment.'
}
$wifiConfigText = [IO.File]::ReadAllText($wifiConfigPath)
foreach ($definitionName in @('WIFI_SSID', 'WIFI_PASSWORD')) {
    $pattern = '(?m)^\s*#define\s+' + [regex]::Escape($definitionName) + '\s+"((?:\\.|[^"\\])*)"\s*(?://[^\r\n]*)?\r?$'
    $definition = [regex]::Match($wifiConfigText, $pattern)
    if (-not $definition.Success -or [string]::IsNullOrWhiteSpace($definition.Groups[1].Value) -or
        $definition.Groups[1].Value -eq 'CHANGE_ME') {
        throw "Set $definitionName to a nonempty C string in wifi_config.h before building for deployment."
    }
}
# Never include the credential values in command arguments or build output.
$wifiConfigText = $null
$definition = $null

$previousSdk = $env:PICO_SDK_PATH
$previousToolchain = $env:PICO_TOOLCHAIN_PATH
try {
    $env:PICO_SDK_PATH = $sdk
    $env:PICO_TOOLCHAIN_PATH = $toolchain
    $buildPath = [IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
    & $cmake -S $projectRoot -B $buildPath -G Ninja '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_MAKE_PROGRAM=$ninja" "-DPython3_EXECUTABLE=$python" '-DPICO_BOARD=pico2_w'
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    & $cmake --build $buildPath --parallel
    if ($LASTEXITCODE -ne 0) { throw "Firmware build failed ($LASTEXITCODE)" }
    Write-Output "UF2: $(Join-Path $buildPath 'hid_usb.uf2')"
} finally {
    $env:PICO_SDK_PATH = $previousSdk
    $env:PICO_TOOLCHAIN_PATH = $previousToolchain
}

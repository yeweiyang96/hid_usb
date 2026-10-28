[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$projectRoot = Split-Path -Parent $PSScriptRoot
$toolRoot = Join-Path $projectRoot '.tools'
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'toolchain.lock.json') -Raw | ConvertFrom-Json
if (-not [Environment]::Is64BitOperatingSystem) { throw 'This toolchain requires 64-bit Windows.' }
if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { throw 'Install Git for Windows before setup.' }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
New-Item -ItemType Directory -Path (Join-Path $toolRoot 'downloads') -Force | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem

foreach ($archive in $lock.archives) {
    $fileName = [IO.Path]::GetFileName(([Uri]$archive.url).AbsolutePath)
    $download = Join-Path (Join-Path $toolRoot 'downloads') $fileName
    if (-not (Test-Path -LiteralPath $download)) {
        Write-Output "Downloading $($archive.name) $($archive.version) from official release..."
        $partial = $download + '.partial'
        Invoke-WebRequest -UseBasicParsing -Uri $archive.url -OutFile $partial
        if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $archive.sha256) {
            throw "SHA256 mismatch for $fileName. The partial file has not been installed."
        }
        Move-Item -LiteralPath $partial -Destination $download
    }
    if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne $archive.sha256) {
        throw "SHA256 mismatch for cached $fileName. Inspect/remove that archive and rerun setup."
    }
    $destination = Join-Path $toolRoot $archive.directory
    $marker = Join-Path $destination '.archive-sha256'
    if (Test-Path -LiteralPath $destination) {
        if (-not (Test-Path -LiteralPath $marker) -or
            ([IO.File]::ReadAllText($marker).Trim() -ne $archive.sha256)) {
            throw "Incomplete/different installation at $destination. Move it aside and rerun setup."
        }
    } else {
        Write-Output "Extracting $($archive.name)..."
        [IO.Compression.ZipFile]::ExtractToDirectory($download, $destination)
        [IO.File]::WriteAllText($marker, $archive.sha256, [Text.Encoding]::ASCII)
    }
    if (-not (Test-Path -LiteralPath (Join-Path $toolRoot $archive.executable))) {
        throw "Missing executable after extraction: $($archive.name)"
    }
}

$sdkPath = Join-Path $toolRoot 'pico-sdk'
if (-not (Test-Path -LiteralPath $sdkPath)) {
    & git clone --depth 1 --branch $lock.sdk.version $lock.sdk.url $sdkPath
    if ($LASTEXITCODE -ne 0) { throw 'Pico SDK clone failed; rerun after checking network access.' }
}
$sdkHead = & git -C $sdkPath rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $sdkHead.Trim() -ne $lock.sdk.commit) { throw 'Pico SDK revision differs from toolchain.lock.json.' }
& git -C $sdkPath submodule update --init --recursive --depth 1
if ($LASTEXITCODE -ne 0) { throw 'Pico SDK submodule fetch failed; rerun setup to finish it.' }
$submodules = & git -C $sdkPath submodule status --recursive
if ($LASTEXITCODE -ne 0 -or @($submodules | Where-Object { $_ -notmatch '^ ' }).Count -gt 0) {
    throw 'Pico SDK submodule versions do not match the pinned SDK.'
}
$submodules | Set-Content -LiteralPath (Join-Path $toolRoot 'sdk-submodules.lock.txt') -Encoding ASCII
Write-Output 'Local toolchain ready. No system PATH or SDK configuration was changed.'
Write-Output 'Compile check: powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -CompileOnly'
Write-Output 'Deployment build: fill local wifi_config.h, then run scripts/build.ps1.'

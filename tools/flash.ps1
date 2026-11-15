# Build + flash from any PowerShell: loads the ESP-IDF environment, then runs flash.py.
#   .\tools\flash.ps1              build + flash
#   .\tools\flash.ps1 --no-build   flash the existing build
# ESP-IDF location: $env:IDF_PATH if set, else ~\esp\esp-idf (see docs/setup.md).
$ErrorActionPreference = "Stop"
$idf = if ($env:IDF_PATH) { $env:IDF_PATH } else { Join-Path $env:USERPROFILE "esp\esp-idf" }
if (-not (Test-Path (Join-Path $idf "export.ps1"))) {
    Write-Error "ESP-IDF not found at $idf (set IDF_PATH or install it, see docs/setup.md)"
}
. (Join-Path $idf "export.ps1") | Out-Null
$repo = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $repo "firmware\sdkconfig"))) {
    idf.py -C (Join-Path $repo "firmware") set-target esp32s3
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
python (Join-Path $PSScriptRoot "flash.py") @args
exit $LASTEXITCODE

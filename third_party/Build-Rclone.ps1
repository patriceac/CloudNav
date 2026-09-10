[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$OutputPath = if ([IO.Path]::IsPathRooted($OutputPath)) { $OutputPath } else { Join-Path $projectRoot $OutputPath }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$cache = Join-Path $projectRoot '.third-party'
function Get-CheckedArchive([string]$Uri, [string]$Path, [string]$Sha256) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
    if (-not (Test-Path -LiteralPath $Path) -or
        (Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant() -ne $Sha256) {
        Invoke-WebRequest -Uri $Uri -OutFile $Path
    }
    if ((Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant() -ne $Sha256) { throw "Archive checksum mismatch: $Path" }
}
$goArchive = Join-Path $cache 'go\go1.27.1.windows-amd64.zip'
Get-CheckedArchive 'https://go.dev/dl/go1.27.1.windows-amd64.zip' $goArchive 'a3911b5e0e1b1053f25ed0675f4c1c6aad1e2bfcf253df2b9be4caabd2edd95d'
$goRoot = Join-Path $cache 'go'
$go = Join-Path $goRoot 'go\bin\go.exe'
if (-not (Test-Path -LiteralPath $go)) { Expand-Archive -LiteralPath $goArchive -DestinationPath $goRoot -Force }
$sourceArchive = Join-Path $cache 'rclone-source\v1.75.0.zip'
Get-CheckedArchive 'https://codeload.github.com/rclone/rclone/zip/refs/tags/v1.75.0' $sourceArchive 'ef5dae8843f8d16e1a7e22455998b980bba327344cb19b4010ae7d2930c69f00'
$sourceParent = Split-Path -Parent $sourceArchive
Expand-Archive -LiteralPath $sourceArchive -DestinationPath $sourceParent -Force
$source = Join-Path $sourceParent 'rclone-1.75.0'
& git -C $projectRoot apply --unidiff-zero --directory=.third-party/rclone-source/rclone-1.75.0 --ignore-space-change --whitespace=nowarn (Join-Path $PSScriptRoot 'rclone-cloudnav.patch')
if ($LASTEXITCODE -ne 0) { throw 'Unable to apply the CloudNav OAuth patch.' }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'rclone-cloudnav_test.go') -Destination (Join-Path $source 'lib\oauthutil\cloudnav_test.go')
New-Item -ItemType Directory -Force (Split-Path -Parent $OutputPath) | Out-Null
$savedCgo = $env:CGO_ENABLED
$savedToolchain = $env:GOTOOLCHAIN
$savedGoos = $env:GOOS
$savedGoarch = $env:GOARCH
try {
    $env:CGO_ENABLED = '0'
    $env:GOTOOLCHAIN = 'local'
    $env:GOOS = 'windows'
    $env:GOARCH = 'amd64'
    Push-Location $source
    try {
        & $go test ./lib/oauthutil -run TestCloudNavReservedPortFallback -count=1
        if ($LASTEXITCODE -ne 0) { throw 'OAuth callback regression test failed.' }
        & $go build -trimpath -buildvcs=false -ldflags '-s -w -X github.com/rclone/rclone/fs.Version=v1.75.0-cloudnav.3' -o $OutputPath .
        if ($LASTEXITCODE -ne 0) { throw 'Patched rclone build failed.' }
    } finally { Pop-Location }
} finally {
    $env:CGO_ENABLED = $savedCgo
    $env:GOTOOLCHAIN = $savedToolchain
    $env:GOOS = $savedGoos
    $env:GOARCH = $savedGoarch
}
Get-FileHash -LiteralPath $OutputPath -Algorithm SHA256

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceRoot = Join-Path $projectRoot 'src'
$testRoot = Join-Path $projectRoot 'tests'
$outputRoot = Join-Path $projectRoot "build\$Configuration"
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

$rcloneVersion = '1.75.0'
$rcloneSha256 = '203581f0a7baeae873f2347483a798c79e2eaf5c384a4e9d866aa374f1c89ac0'
$rcloneExeSha256 = '8be30f02266a6eaad9d481941ef287b9744bb7140034b097ad862a2ccff3e24c'
$rcloneCache = Join-Path $projectRoot ".third-party\rclone\v$rcloneVersion"
$rcloneExe = Join-Path $rcloneCache 'rclone.exe'
$rcloneReady = (Test-Path -LiteralPath $rcloneExe) -and
    ((Get-FileHash -LiteralPath $rcloneExe -Algorithm SHA256).Hash.ToLowerInvariant() -eq $rcloneExeSha256)
if (-not $rcloneReady) {
    New-Item -ItemType Directory -Force -Path $rcloneCache | Out-Null
    $archive = Join-Path $rcloneCache "rclone-v$rcloneVersion-windows-amd64.zip"
    Invoke-WebRequest -UseBasicParsing -Uri "https://downloads.rclone.org/v$rcloneVersion/rclone-v$rcloneVersion-windows-amd64.zip" -OutFile $archive
    $actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $rcloneSha256) {
        throw 'The downloaded rclone archive does not have the expected SHA-256 checksum.'
    }
    $expanded = Join-Path $rcloneCache 'expanded'
    Expand-Archive -LiteralPath $archive -DestinationPath $expanded -Force
    $downloadedExe = Join-Path $expanded "rclone-v$rcloneVersion-windows-amd64\rclone.exe"
    if (-not (Test-Path -LiteralPath $downloadedExe)) {
        throw 'Le téléchargement rclone ne contient pas le fichier attendu.'
    }
    Copy-Item -LiteralPath $downloadedExe -Destination $rcloneExe
}
if ((Get-FileHash -LiteralPath $rcloneExe -Algorithm SHA256).Hash.ToLowerInvariant() -ne $rcloneExeSha256) {
    throw 'The extracted rclone.exe does not have the expected SHA-256 checksum.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Build Tools est introuvable.'
}
$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) {
    throw 'Le compilateur C++ x64 de Visual Studio est introuvable.'
}
$developerShell = Join-Path $visualStudio 'Common7\Tools\VsDevCmd.bat'

$compileOptions = if ($Configuration -eq 'Release') {
    '/nologo /std:c++17 /utf-8 /EHsc /W4 /WX /O2 /GL /MT /DUNICODE /D_UNICODE /DNDEBUG'
} else {
    '/nologo /std:c++17 /utf-8 /EHsc /W4 /WX /Od /Zi /MTd /DUNICODE /D_UNICODE /D_DEBUG'
}
$linkOptions = if ($Configuration -eq 'Release') {
    '/link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup'
} else {
    '/link /DEBUG /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup'
}
$testCompileOptions = $compileOptions -replace ' /DNDEBUG', ''

$resourceOutput = Join-Path $outputRoot 'CloudNav.res'
$executableOutput = Join-Path $outputRoot 'CloudNav.exe'
$testOutput = Join-Path $outputRoot 'CloudNavTests.exe'
$windowPositionTestOutput = Join-Path $outputRoot 'CloudNavWindowPositionTests.exe'
$uiTestOutput = Join-Path $outputRoot 'CloudNavUiTests.exe'
$clientTestOutput = Join-Path $outputRoot 'CloudNavClientTests.exe'

$commands = @(
    "rc.exe /nologo /fo `"$resourceOutput`" `"$(Join-Path $sourceRoot 'CloudNav.rc')`"",
    "cl.exe $compileOptions /Fe:`"$executableOutput`" `"$(Join-Path $sourceRoot 'CloudNav.cpp')`" `"$(Join-Path $sourceRoot 'folder_manager.cpp')`" `"$(Join-Path $sourceRoot 'migration.cpp')`" `"$(Join-Path $sourceRoot 'client_management.cpp')`" `"$resourceOutput`" $linkOptions",
    "cl.exe $testCompileOptions /Fe:`"$testOutput`" `"$(Join-Path $testRoot 'LogicTests.cpp')`" /link /SUBSYSTEM:CONSOLE",
    "cl.exe $testCompileOptions /Fe:`"$windowPositionTestOutput`" `"$(Join-Path $testRoot 'WindowPositionIntegration.cpp')`" advapi32.lib user32.lib /link /SUBSYSTEM:CONSOLE",
    "cl.exe $testCompileOptions /Fe:`"$uiTestOutput`" `"$(Join-Path $testRoot 'UiIntegration.cpp')`" user32.lib /link /SUBSYSTEM:CONSOLE",
    "cl.exe $testCompileOptions /Fe:`"$clientTestOutput`" `"$(Join-Path $testRoot 'ClientRuntimeTests.cpp')`" `"$(Join-Path $sourceRoot 'client_management.cpp')`" /link /SUBSYSTEM:CONSOLE"
) -join ' && '

$cmdLine = "`"$developerShell`" -no_logo -arch=x64 -host_arch=x64 && $commands"
& $env:ComSpec /d /s /c $cmdLine
if ($LASTEXITCODE -ne 0) {
    throw "La compilation a échoué (code $LASTEXITCODE)."
}

& $testOutput
if ($LASTEXITCODE -ne 0) {
    throw "Les tests ont échoué (code $LASTEXITCODE)."
}

Get-Item -LiteralPath $executableOutput, $testOutput, $windowPositionTestOutput, $uiTestOutput, $clientTestOutput |
    Select-Object Name, Length, LastWriteTime, FullName

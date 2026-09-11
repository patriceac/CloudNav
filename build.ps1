[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$GoogleOAuthConfigPath = $env:CLOUDNAV_GOOGLE_OAUTH_CONFIG_PATH,
    [string]$GoogleOAuthRemote = 'gdrive'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceRoot = Join-Path $projectRoot 'src'
$testRoot = Join-Path $projectRoot 'tests'
$outputRoot = Join-Path $projectRoot "build\$Configuration"
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

$generatedRoot = Join-Path $outputRoot 'generated'
New-Item -ItemType Directory -Force -Path $generatedRoot | Out-Null
$googleClientId = ''
$googleClientSecret = ''
if ($GoogleOAuthConfigPath) {
    if (-not (Test-Path -LiteralPath $GoogleOAuthConfigPath)) {
        throw "Google OAuth configuration was not found: $GoogleOAuthConfigPath"
    }
    $sections = @{}
    $section = ''
    foreach ($line in Get-Content -LiteralPath $GoogleOAuthConfigPath) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^\[([^]]+)\]$') {
            $section = $Matches[1]
            if (-not $sections.ContainsKey($section)) { $sections[$section] = @{} }
        } elseif ($section -and $trimmed -match '^([^#;][^=]+?)\s*=\s*(.*)$') {
            $sections[$section][$Matches[1].Trim()] = $Matches[2].Trim()
        }
    }
    $candidate = $sections[$GoogleOAuthRemote]
    if ($candidate -and $candidate['type'] -ne 'drive') {
        throw "OAuth source remote '$GoogleOAuthRemote' is not a Google Drive remote."
    }
    if ($candidate) {
        $googleClientId = $candidate['client_id']
        $googleClientSecret = $candidate['client_secret']
    }
}
if ($Configuration -eq 'Release' -and (-not $googleClientId -or -not $googleClientSecret)) {
    throw 'Release builds require -GoogleOAuthConfigPath with a dedicated Google Drive desktop client.'
}
if ($googleClientId -and ($googleClientId -notmatch '^[0-9]+-[A-Za-z0-9_-]+\.apps\.googleusercontent\.com$' -or
    $googleClientSecret -notmatch '^[A-Za-z0-9_-]+$')) {
    throw 'The Google OAuth configuration contains an invalid desktop client.'
}
$oauthHeader = @"
#pragma once
namespace cloudnav {
inline constexpr wchar_t kGoogleOAuthClientId[] = L"$googleClientId";
inline constexpr wchar_t kGoogleOAuthClientSecret[] = L"$googleClientSecret";
}
"@
[IO.File]::WriteAllText((Join-Path $generatedRoot 'google_oauth_config.h'), $oauthHeader, [Text.UTF8Encoding]::new($false))

$rcloneVersion = '1.75.0-cloudnav.3'
$rcloneExeSha256 = '7a2c1e8ed2ab58b588b89004f7787a0aae13d5a80318eabef8d7f5e8420b2ae9'
$rcloneCache = Join-Path $projectRoot ".third-party\rclone\v$rcloneVersion"
$rcloneExe = Join-Path $rcloneCache 'rclone.exe'
$rcloneReady = (Test-Path -LiteralPath $rcloneExe) -and
    ((Get-FileHash -LiteralPath $rcloneExe -Algorithm SHA256).Hash.ToLowerInvariant() -eq $rcloneExeSha256)
if (-not $rcloneReady) {
    & (Join-Path $projectRoot 'third_party\Build-Rclone.ps1') -OutputPath $rcloneExe
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
$compileOptions += " /I`"$generatedRoot`""
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
$authFixtureOutput = Join-Path $outputRoot 'CloudNavAuthFixture.exe'

$commands = @(
    "rc.exe /nologo /fo `"$resourceOutput`" `"$(Join-Path $sourceRoot 'CloudNav.rc')`"",
    "cl.exe $compileOptions /Fe:`"$executableOutput`" `"$(Join-Path $sourceRoot 'CloudNav.cpp')`" `"$(Join-Path $sourceRoot 'folder_manager.cpp')`" `"$(Join-Path $sourceRoot 'migration.cpp')`" `"$(Join-Path $sourceRoot 'client_management.cpp')`" `"$resourceOutput`" $linkOptions",
    "cl.exe $testCompileOptions /Fe:`"$testOutput`" `"$(Join-Path $testRoot 'LogicTests.cpp')`" /link /SUBSYSTEM:CONSOLE",
    "cl.exe $testCompileOptions /Fe:`"$windowPositionTestOutput`" `"$(Join-Path $testRoot 'WindowPositionIntegration.cpp')`" advapi32.lib user32.lib /link /SUBSYSTEM:CONSOLE",
    "cl.exe $testCompileOptions /Fe:`"$uiTestOutput`" `"$(Join-Path $testRoot 'UiIntegration.cpp')`" user32.lib /link /SUBSYSTEM:CONSOLE",
    "cl.exe $testCompileOptions /Fe:`"$clientTestOutput`" `"$(Join-Path $testRoot 'ClientRuntimeTests.cpp')`" `"$(Join-Path $sourceRoot 'client_management.cpp')`" /link /SUBSYSTEM:CONSOLE",
    "cl.exe $testCompileOptions /Fe:`"$authFixtureOutput`" `"$(Join-Path $testRoot 'AuthRuntimeFixture.cpp')`" /link /SUBSYSTEM:CONSOLE"
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

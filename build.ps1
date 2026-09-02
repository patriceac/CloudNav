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

$commands = @(
    "rc.exe /nologo /fo `"$resourceOutput`" `"$(Join-Path $sourceRoot 'CloudNav.rc')`"",
    "cl.exe $compileOptions /Fe:`"$executableOutput`" `"$(Join-Path $sourceRoot 'CloudNav.cpp')`" `"$(Join-Path $sourceRoot 'folder_manager.cpp')`" `"$resourceOutput`" $linkOptions",
    "cl.exe $testCompileOptions /Fe:`"$testOutput`" `"$(Join-Path $testRoot 'LogicTests.cpp')`" /link /SUBSYSTEM:CONSOLE",
    "cl.exe $testCompileOptions /Fe:`"$windowPositionTestOutput`" `"$(Join-Path $testRoot 'WindowPositionIntegration.cpp')`" advapi32.lib user32.lib /link /SUBSYSTEM:CONSOLE"
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

Get-Item -LiteralPath $executableOutput, $testOutput, $windowPositionTestOutput |
    Select-Object Name, Length, LastWriteTime, FullName

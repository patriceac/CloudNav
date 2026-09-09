[CmdletBinding()]
param(
    [ValidateSet('Ui', 'Migration', 'MigrationResume', 'MigrationEngine', 'MigrationReport', 'OneDrive', 'Failure', 'WindowPosition')]
    [string[]]$Scenario = @('Ui', 'Migration', 'MigrationResume', 'MigrationEngine', 'MigrationReport', 'OneDrive', 'Failure', 'WindowPosition'),
    [switch]$SkipBuild,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $SkipBuild) { & (Join-Path $projectRoot 'build.ps1') -Configuration Release }
$releasePath = Join-Path $projectRoot 'build\Release'
$runner = Join-Path $env:USERPROFILE '.agents\skills\hyperv-test-executables\scripts\Invoke-HyperVExecutableTest.ps1'
if (-not (Test-Path -LiteralPath $runner)) { throw 'The isolated Hyper-V test harness is required.' }
$checkpointRoot = Join-Path $projectRoot 'output\release-checks'
New-Item -ItemType Directory -Force -Path $checkpointRoot | Out-Null
$cases = @{
    Ui = @{ Exe = 'CloudNavUiTests.exe'; Args = '"{OUTDIR}\ui-result.json"'; Actions = 'vm-ui-actions.json'; Result = 'ui-result.json'; Images = @('ui-main.png','ui-unverified.png','ui-folder-preview.png','ui-folder-confirm.png','ui-folder-applied.png') }
    Migration = @{ Exe = 'CloudNav.exe'; Args = '--demo-migration "{OUTDIR}\migration-result.json"'; Actions = 'vm-migration-actions.json'; Result = 'migration-result.json'; Images = @('migration-analysis-progress.png','migration-analysis-complete.png','migration-copy-progress.png','migration-verified.png','migration-cutover.png') }
    MigrationResume = @{ Exe = 'CloudNav.exe'; Args = '--demo-migration "{OUTDIR}\migration-result.json"'; Actions = 'vm-migration-resume-actions.json'; Result = 'migration-result.json'; Images = @('migration-cancelled.png','migration-resumed-verified.png') }
    MigrationEngine = @{ Exe = 'CloudNav.exe'; Args = '--self-test-embedded-rclone "{OUTDIR}\rclone-result.json"'; Actions = 'vm-rclone-self-test-actions.json'; Result = 'rclone-result.json'; Images = @() }
    MigrationReport = @{ Exe = 'CloudNavUiTests.exe'; Args = '"{OUTDIR}\ui-result.json" --migration-report'; Actions = 'vm-ui-actions.json'; Result = 'ui-result.json'; Images = @('report-summary.png','report-reverse.png','report-bidirectional.png','report-all.png','report-new.png','report-partial.png') }
    OneDrive = @{ Exe = 'CloudNav.exe'; Args = '--demo-safe-onedrive'; Actions = 'vm-onedrive-actions.json'; Images = @('cloudnav-onedrive-uninstall-confirm.png','cloudnav-onedrive-uninstall-simulated.png') }
    Failure = @{ Exe = 'CloudNav.exe'; Args = '--demo-repoint-failure'; Actions = 'vm-repoint-failure-actions.json'; Images = @('cloudnav-112-repoint-error.png','cloudnav-112-repoint-status.png') }
    WindowPosition = @{ Exe = 'CloudNavWindowPositionTests.exe'; Args = '"{OUTDIR}\window-position-result.json"'; Actions = 'vm-window-position-actions.json'; Result = 'window-position-result.json'; Images = @() }
}

foreach ($name in $Scenario) {
    $case = $cases[$name]
    $actionsPath = Join-Path $PSScriptRoot $case.Actions
    $identityFiles = @((Join-Path $releasePath 'CloudNav.exe'), (Join-Path $releasePath $case.Exe), $actionsPath, $PSCommandPath, $runner)
    $identity = ($identityFiles | ForEach-Object { (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }) -join ':'
    $checkpointPath = Join-Path $checkpointRoot ($name + '.json')
    if (-not $Force -and (Test-Path -LiteralPath $checkpointPath)) {
        $checkpoint = Get-Content -LiteralPath $checkpointPath -Raw | ConvertFrom-Json
        if ($checkpoint.Identity -eq $identity -and $checkpoint.Passed -and (Test-Path -LiteralPath $checkpoint.BrokerResultPath)) {
            $stored = Get-Content -LiteralPath $checkpoint.BrokerResultPath -Raw | ConvertFrom-Json
            $storedGuestPath = Join-Path (Split-Path -Parent $checkpoint.BrokerResultPath) 'result.json'
            $imagesPresent = @($case.Images | Where-Object { -not (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $checkpoint.BrokerResultPath) $_)) }).Count -eq 0
            if ($stored.HarnessSucceeded -and $stored.TestPassed -and $stored.PayloadChildDeleted -and
                $stored.VmFinalState -eq 'Off' -and $imagesPresent -and (Test-Path -LiteralPath $storedGuestPath)) {
                Write-Host "$name`: reusing the passed check for this exact artifact and scenario."
                continue
            }
        }
    }
    $parameters = @{
        ArtifactPath = $releasePath
        ExecutableRelativePath = $case.Exe
        Arguments = $case.Args
        ActionsPath = $actionsPath
        NetworkProfile = 'None'
        ExecutionTimeoutSeconds = 180
        ThrowOnFailure = $true
    }
    if ($case.Result) {
        $parameters.AssertResultFile = '{OUTDIR}\' + $case.Result
        $parameters.AssertResultJsonPointer = '/passed'
        $parameters.AssertResultEqualsJson = 'true'
    }
    $run = (& $runner @parameters) | ConvertFrom-Json
    $broker = Get-Content -LiteralPath $run.BrokerResultPath -Raw | ConvertFrom-Json
    $guest = Get-Content -LiteralPath $run.GuestResultPath -Raw | ConvertFrom-Json
    if (-not ($run.OverallSucceeded -and $run.TestEvaluated -and $run.TestPassed -and
        $broker.HarnessSucceeded -and $guest.HarnessSucceeded -and $guest.ProcessCleanup.Success -and
        $broker.VmFinalState -eq 'Off' -and $broker.PayloadChildDeleted -and
        -not (Test-Path -LiteralPath $broker.PayloadChildVhdx)) -or $broker.EvidenceWarnings.Count -gt 0) {
        throw "$name failed its assertion or evidence/cleanup checks. See $($run.ResultPath)"
    }
    foreach ($imageName in $case.Images) {
        if (-not (Test-Path -LiteralPath (Join-Path $run.ResultPath $imageName))) { throw "Missing screenshot: $imageName" }
    }
    [ordered]@{ Scenario = $name; Identity = $identity; Passed = $true; BrokerResultPath = $run.BrokerResultPath; ResultPath = $run.ResultPath; CompletedUtc = [DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $checkpointPath -Encoding utf8
    Write-Host "$name passed. Evidence: $($run.ResultPath)"
}

Write-Host 'Requested isolated release checks passed. Visually inspect the recorded screenshots before release.'

[CmdletBinding()]
param(
    [string]$TaskName = 'OneDrive-GDrive-Bidirectional-Sync',
    [string]$TaskPath = '\',
    [string]$ExecutablePath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\CloudNav.exe'),
    [ValidateRange(0,100)][int]$MaxDeletePercent = 10,
    [string]$AccessMarker = '.onedrive-gdrive-sync-access'
)
$ErrorActionPreference = 'Stop'
$app = Get-Item -LiteralPath $ExecutablePath
if ([version]$app.VersionInfo.ProductVersion -lt [version]'1.4.9') { throw 'CloudNav 1.4.9 or later is required.' }
$existing = Get-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath
if ($existing.State -eq 'Running') { throw 'The existing sync is running; wait for it to finish before updating.' }
if ($AccessMarker -match '["\r\n]') { throw 'Invalid access marker.' }
$backupDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) 'work\scheduled-task'
New-Item -ItemType Directory -Path $backupDirectory -Force | Out-Null
$backup = Join-Path $backupDirectory ('before-{0}.xml' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
Export-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath | Set-Content -LiteralPath $backup -Encoding Unicode
$arguments = '--sync --max-delete-percent {0}' -f $MaxDeletePercent
if ($AccessMarker) { $arguments += ' --check-access "{0}"' -f $AccessMarker }
$action = New-ScheduledTaskAction -Execute $app.FullName -Argument $arguments -WorkingDirectory $app.DirectoryName
# Preserve triggers, principal, missed-run behavior, network policy and time limit.
$settings = $existing.Settings
$settings.MultipleInstances = 2 # TASK_INSTANCES_IGNORE_NEW
Set-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath -Action $action -Settings $settings | Out-Null
$updated = Get-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath
if ($updated.Actions.Execute -ne $app.FullName -or $updated.Actions.Arguments -ne $arguments) { throw 'Scheduled action verification failed.' }
[pscustomobject]@{ Task=$TaskName; Executable=$app.FullName; Arguments=$arguments; Backup=$backup }

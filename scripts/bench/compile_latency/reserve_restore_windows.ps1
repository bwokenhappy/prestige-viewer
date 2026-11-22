param([Parameter(Mandatory=$true)][ValidateSet('Reserve','Restore')][string]$Action)
# Invoke Reserve ONLY after the preceding worker's explicit handoff.
# No scheduled-task creation, privilege changes, driver cache purge or owner login.
$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'
$root='C:\sltest\astra-shader-compile'
$token='astra-shader-compile-amd-ae1f063eb7-20261119'
$lock='C:\sltest\BOXLOCK.txt'
function Assert-Idle {
 if (@(Get-Process -ErrorAction SilentlyContinue | Where-Object {$_.ProcessName -match 'Slipstream|Alchemy|SLPlugin'}).Count) { throw 'viewer/worker present' }
 if ((Get-ScheduledTask -TaskName SLRun).State -eq 'Running') { throw 'desktop task active' }
}
function Digest([string]$path) { return (Get-FileHash $path).Hash.ToLowerInvariant() }
if ($Action -eq 'Reserve') {
 Assert-Idle
 if (Test-Path $root) { throw 'owned root already exists; inspect prior state before reuse' }
 $stream=[IO.File]::Open($lock,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
 try {
  $bytes=[Text.Encoding]::UTF8.GetBytes($token+"`n");$stream.Write($bytes,0,$bytes.Length)
 } finally { $stream.Dispose() }
 Assert-Idle
 New-Item -ItemType Directory "$root\bak","$root\out","$root\home\local","$root\home\roam\SlipstreamViewer\user_settings" | Out-Null
 $paths=@('C:\sltest\job.ps1','C:\sltest\job.log','C:\sltest\job.done',
   (Join-Path $env:APPDATA 'SlipstreamViewer\user_settings\settings.xml'),
   (Join-Path $env:APPDATA 'SlipstreamViewer\user_settings\ignorable_dialogs.xml'),
   (Join-Path $env:APPDATA 'Slipstream\user_settings\settings.xml'),
   (Join-Path $env:APPDATA 'Slipstream\user_settings\ignorable_dialogs.xml'))
 $state=@();$i=0
 foreach ($p in $paths) {
  $row=@{path=$p;exists=(Test-Path $p);backup="$root\bak\original-$i";shared_job=($i -lt 3)}
  if ($row.exists) {
   $file=Get-Item $p -Force
   if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'unexpected reparse point in owner backup' }
   Copy-Item $p $row.backup
   $row.sha256=Digest $p
   if ((Digest $row.backup) -ne $row.sha256) { throw 'backup mismatch' }
  }
  $state += $row; $i++
 }
 $state | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 "$root\bak\original-state.json"
 $settings=$paths[3]
 if (!(Test-Path $settings)) { throw 'missing source settings template' }
 Copy-Item $settings "$root\bak\isolated-settings-template.xml"
 Copy-Item $settings "$root\home\roam\SlipstreamViewer\user_settings\settings.xml"
 if (Test-Path $paths[4]) { Copy-Item $paths[4] "$root\home\roam\SlipstreamViewer\user_settings\ignorable_dialogs.xml" }
 Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,DriverDate,PNPDeviceID |
  ConvertTo-Json | Set-Content -Encoding UTF8 "$root\out\driver-identity.json"
 'reserved; backups complete; no viewer launched'
} else {
 if (!(Test-Path $lock) -or (Get-Content $lock -Raw).Trim() -ne $token) { throw 'reservation not owned' }
 Assert-Idle
 $state=Get-Content "$root\bak\original-state.json" -Raw | ConvertFrom-Json
 # Check all backup and owner-preference evidence before restoring any shared job file.
 foreach ($row in $state) {
  if ($row.exists -and (Digest $row.backup) -ne $row.sha256) { throw 'backup changed' }
  if (!$row.shared_job) {
   if ((Test-Path $row.path) -ne $row.exists) { throw 'owner preference existence changed' }
   if ($row.exists -and (Digest $row.path) -ne $row.sha256) { throw 'owner preferences unexpectedly changed' }
  }
 }
 $job=Get-Content 'C:\sltest\job.ps1' -Raw
 if (!$job.Contains($root) -and (Digest 'C:\sltest\job.ps1') -ne $state[0].sha256) { throw 'job no longer ours or original' }
 foreach ($row in ($state | Where-Object {$_.shared_job})) {
  if ($row.exists) { Copy-Item $row.backup $row.path -Force }
  elseif (Test-Path $row.path) { Remove-Item $row.path }
 }
 foreach ($row in $state) {
  if ((Test-Path $row.path) -ne $row.exists) { throw 'restore existence mismatch' }
  if ($row.exists -and (Digest $row.path) -ne $row.sha256) { throw 'restore hash mismatch' }
 }
 Assert-Idle
 if ((Get-Content $lock -Raw).Trim() -ne $token) { throw 'reservation changed' }
 Remove-Item $lock
 @{owner_preferences_unchanged=$true;shared_job_files_exactly_restored=$true;
   viewer_and_task_idle=$true;only_owned_lock_released=$true;utc=[DateTime]::UtcNow.ToString('o')} |
  ConvertTo-Json | Set-Content -Encoding UTF8 "$root\out\RESTORED.json"
 Get-Content "$root\out\RESTORED.json"
}

param([Parameter(Mandatory=$true)][string]$List,
      [Parameter(Mandatory=$true)][string]$Out,
      [string]$Root = 'C:\sltest\astra')
# Adapted from the private rtperf-agent bench runner. Caller owns the atomic
# BOXLOCK reservation and restores the desktop job wrapper after completion.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$token = 'astra-perf-architecture-ae1f063eb7-20261118'
$lock = 'C:\sltest\BOXLOCK.txt'
if (!(Test-Path $lock) -or (Get-Content $lock -Raw).Trim() -ne $token) { throw 'reservation not owned' }
function ViewerRunning { return [bool](Get-Process -ErrorAction SilentlyContinue | Where-Object {$_.ProcessName -match 'Slipstream|Alchemy'}) }
if (ViewerRunning) { throw 'foreign viewer running' }
$env:APPDATA = "$Root\home\roam"
$env:LOCALAPPDATA = "$Root\home\local"
$log = "$env:APPDATA\SlipstreamViewer\logs\Slipstream.log"
$settings = "$env:APPDATA\SlipstreamViewer\user_settings\settings.xml"
$template = "$Root\bak\isolated-settings-template.xml"
if (!(Test-Path $template)) { Copy-Item $settings $template }
New-Item -ItemType Directory -Force $Out | Out-Null
function Note($s) { ([DateTime]::UtcNow.ToString('o') + ' ' + $s) | Out-File "$Out\progress.txt" -Append -Encoding utf8 }
try {
 foreach ($run in (Get-Content $List -Raw | ConvertFrom-Json)) {
  if (Test-Path "$Root\STOP") { throw 'STOP requested' }
  if ((Get-Content $lock -Raw).Trim() -ne $token) { throw 'reservation changed' }
  if (ViewerRunning) { throw 'unexpected viewer; refusing run' }
  if (Test-Path "$Out\$($run.name).json") { throw 'run output exists; choose a fresh output directory' }
  Copy-Item $template $settings -Force
  Remove-Item $log -ErrorAction SilentlyContinue
  $exe = Join-Path $run.package 'SlipstreamViewer.exe'
  $identity = @{source=$run.source; executable_sha256=(Get-FileHash $exe).Hash;
   capture_manifest_sha256=(Get-FileHash (Join-Path $run.capture 'manifest.json')).Hash;
   path_sha256=(Get-FileHash $run.path).Hash; settings=$run.settings;
   settings_template_sha256=(Get-FileHash $template).Hash}
  $identity | ConvertTo-Json -Depth 8 | Set-Content "$Out\$($run.name).identity.json"
  $a = @('--bench',$run.capture,'--bench-path',$run.path,'--bench-loops','2','--bench-report',"$Out\$($run.name).json")
  foreach ($prop in ($run.settings.PSObject.Properties | Sort-Object Name)) { $a += @('--set',$prop.Name,[string]$prop.Value) }
  Note "launch $($run.name)"
  $proc = Start-Process -FilePath $exe -WorkingDirectory $run.package -ArgumentList $a -PassThru
  $done = $proc.WaitForExit(1200000)
  if (!$done) { Stop-Process -Id $proc.Id -Force; throw "owned run timed out: $($run.name)" }
  $proc.Refresh()
  if (Test-Path $log) { Copy-Item $log "$Out\$($run.name).log" }
  Note "exit $($run.name) code=$($proc.ExitCode) report=$(Test-Path "$Out\$($run.name).json")"
  if ($proc.ExitCode -ne 0) { throw "viewer failed: $($run.name)" }
  if (!(Test-Path "$Out\$($run.name).json")) { throw 'missing report' }
  $report = Get-Content "$Out\$($run.name).json" -Raw | ConvertFrom-Json
  if (!$report.ok) { throw 'benchmark rejected run' }
 }
 'complete' | Set-Content "$Out\DONE"
} catch {
 Note ('BLOCKED ' + $_.Exception.Message)
 $_.Exception.Message | Set-Content "$Out\BLOCKED"
 exit 2
}

param([Parameter(Mandatory=$true)][string]$List,
      [Parameter(Mandatory=$true)][string]$Out,
      [string]$Root = 'C:\sltest\sol-depth-prepass')
# Adapted from the private rtperf-agent bench runner. Caller owns the atomic
# BOXLOCK reservation and restores the desktop job wrapper after completion.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$token = 'sol-depth-prepass-ae1f063eb7-20261119'
$lock = 'C:\sltest\BOXLOCK.txt'
if (!(Test-Path $lock) -or (Get-Content $lock -Raw).Trim() -ne $token) { throw 'reservation not owned' }
function ViewerRunning { return [bool](Get-Process -ErrorAction SilentlyContinue | Where-Object {$_.ProcessName -match 'Slipstream|Alchemy'}) }
if (ViewerRunning) { throw 'foreign viewer running' }
$env:APPDATA = "$Root\home\roam"
$env:LOCALAPPDATA = "$Root\home\local"
$log = "$env:APPDATA\SlipstreamViewer\logs\Slipstream.log"
$settings = "$env:APPDATA\SlipstreamViewer\user_settings\settings.xml"
$template = "$Root\backup\settings.xml"
if (!(Test-Path $template)) { Copy-Item $settings $template }
New-Item -ItemType Directory -Force $Out | Out-Null
function Note($s) { ([DateTime]::UtcNow.ToString('o') + ' ' + $s) | Out-File "$Out\progress.txt" -Append -Encoding utf8 }
$proc=$null
try {
 foreach ($run in (Get-Content $List -Raw | ConvertFrom-Json)) {
  if ($run.name -isnot [string] -or $run.package -isnot [string] -or $run.capture -isnot [string] -or $run.path -isnot [string]) { throw 'matrix must be a flat array of run objects with scalar paths' }
  if (Test-Path "$Root\STOP") { throw 'STOP requested' }
  if ((Get-Content $lock -Raw).Trim() -ne $token) { throw 'reservation changed' }
  if (ViewerRunning) { throw 'unexpected viewer; refusing run' }
  if (Test-Path "$Out\$($run.name).json") { throw 'run output exists; choose a fresh output directory' }
  $until=[DateTime]::UtcNow.AddHours(2)
  while (!(Test-Path "$($run.package)/READY")) {
   if ((Get-Content $lock -Raw).Trim() -ne $token -or (ViewerRunning) -or (Test-Path "$Root/STOP")) { throw 'reservation/viewer/STOP changed waiting for package' }
   if ([DateTime]::UtcNow -gt $until) { throw 'package readiness timeout' }
   Start-Sleep -Seconds 3
  }
  $run.source=(Get-Content "$($run.package)/source.txt" -Raw).Trim()
  Copy-Item $template $settings -Force
  Remove-Item $log -ErrorAction SilentlyContinue
  $exe = Join-Path $run.package 'SlipstreamViewer.exe'
  $identity = @{source=$run.source; executable_sha256=(Get-FileHash $exe).Hash;
   capture_manifest_sha256=(Get-FileHash (Join-Path $run.capture 'manifest.json')).Hash;
   path_sha256=(Get-FileHash $run.path).Hash; settings=$run.settings;
   settings_template_sha256=(Get-FileHash $template).Hash}
  $identity | ConvertTo-Json -Depth 8 | Set-Content "$Out\$($run.name).identity.json"
  $a = @('--bench',$run.capture,'--bench-path',$run.path,'--bench-loops','1','--bench-report',"$Out\$($run.name).json")
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
  # The process handle may signal before an owned crash/child process has exited.
  # Wait for teardown, but never accept or terminate a foreign viewer.
  $packagePath=[IO.Path]::GetFullPath($run.package).TrimEnd('\')+'\'
  $settleUntil=[DateTime]::UtcNow.AddSeconds(30)
  do {
   $remaining=@(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match 'Slipstream|Alchemy' })
   foreach ($child in $remaining) {
    $childPath=$null
    try { $childPath=$child.Path } catch { }
    # An exiting process can temporarily have no readable image path. Keep waiting
    # for it to disappear; never classify that snapshot as a foreign live viewer.
    if ($childPath) {
     $normalPath=[IO.Path]::GetFullPath($childPath)
     if ($normalPath.StartsWith('\\?\')) { $normalPath=$normalPath.Substring(4) }
     if (!$normalPath.StartsWith($packagePath,[StringComparison]::OrdinalIgnoreCase)) { throw "foreign viewer during teardown (PID $($child.Id))" }
    }
   }
   if (!$remaining.Count) { break }
   Start-Sleep -Milliseconds 200
  } while ([DateTime]::UtcNow -lt $settleUntil)
  if ($remaining.Count) { throw 'owned viewer teardown did not finish' }
 }
 'complete' | Set-Content "$Out\DONE"
} catch {
 if ($proc -and !$proc.HasExited -and $proc.Path -eq $exe) { Stop-Process -Id $proc.Id -Force }
 Note ('BLOCKED ' + $_.Exception.Message)
 $_.Exception.Message | Set-Content "$Out\BLOCKED"
 exit 2
}

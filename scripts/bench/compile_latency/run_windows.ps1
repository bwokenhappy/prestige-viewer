param([Parameter(Mandatory=$true)][string]$List,
      [Parameter(Mandatory=$true)][string]$Out,
      [string]$Root = 'C:\sltest\astra-shader-compile')
# Offline replay only. Run through the existing desktop task after reservation.
# Driver caches are not purged: cold below means the isolated APPLICATION cache.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$token = 'astra-shader-compile-amd-ae1f063eb7-20261119'
$lock = 'C:\sltest\BOXLOCK.txt'
function Assert-Lock {
 if (!(Test-Path $lock) -or (Get-Content $lock -Raw).Trim() -ne $token) { throw 'reservation not owned' }
}
function Viewer-Running {
 return [bool](Get-Process -ErrorAction SilentlyContinue | Where-Object {$_.ProcessName -match 'Slipstream|Alchemy|SLPlugin'})
}
function Write-Json($value, [string]$path) {
 $value | ConvertTo-Json -Depth 14 | Set-Content -Encoding UTF8 $path
}
function Cache-Manifest([string]$directory) {
 $result = @{}
 if (Test-Path $directory) {
  foreach ($f in (Get-ChildItem $directory -Filter 'vk_*' -File)) {
   $result[$f.Name] = @{bytes=$f.Length; sha256=(Get-FileHash $f.FullName).Hash.ToLowerInvariant()}
  }
 }
 return $result
}
function Assert-OwnedPath([string]$path) {
 $full = [IO.Path]::GetFullPath($path)
 if (!$full.StartsWith($Root.TrimEnd('\')+'\', [StringComparison]::OrdinalIgnoreCase)) { throw 'path outside owned root' }
 for ($p=$full; $p.Length -ge $Root.Length; $p=Split-Path $p -Parent) {
  if ((Test-Path $p) -and ((Get-Item $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'owned path has reparse point' }
 }
}
Assert-Lock
if (Viewer-Running) { throw 'viewer already running' }
Assert-OwnedPath $Out
if (Test-Path $Out) { throw 'output exists; use a fresh directory' }
$env:APPDATA = "$Root\home\roam"
$env:LOCALAPPDATA = "$Root\home\local"
foreach ($p in @($env:APPDATA,$env:LOCALAPPDATA)) {
 Assert-OwnedPath $p
 if (!(Test-Path $p)) { throw 'isolated home must exist before launch; prevent Windows folder fallback' }
}
$cache = "$Root\home\app-cache"
$settings = "$env:APPDATA\SlipstreamViewer\user_settings\settings.xml"
$log = "$env:APPDATA\SlipstreamViewer\logs\Slipstream.log"
$template = "$Root\bak\isolated-settings-template.xml"
if (!(Test-Path $template)) { throw 'missing settings template' }
# Do not inherit an unrelated worker or Vulkan-driver override from the desktop task.
Remove-Item Env:SL_VK_PIPELINE_WORKER -ErrorAction SilentlyContinue
New-Item -ItemType Directory $Out | Out-Null
function Note([string]$message) {
 ([DateTime]::UtcNow.ToString('o')+' '+$message) | Out-File "$Out\progress.txt" -Append -Encoding utf8
}
try {
 foreach ($run in (Get-Content $List -Raw | ConvertFrom-Json)) {
  Assert-Lock
  if ((Test-Path "$Root\STOP") -or (Viewer-Running)) { throw 'STOP or unexpected viewer' }
  Assert-OwnedPath $run.package
  if ($run.name -notmatch '^[a-zA-Z0-9_-]+$') { throw 'invalid run name' }
  $prefix = "$Out\$($run.name)"
  if ($run.cold -or $run.seed_directory) {
   if (Test-Path $cache) { Move-Item $cache "$prefix.cache-before" }
  }
  if ($run.seed_directory) {
   $seed = Join-Path $Root $run.seed_directory
   Assert-OwnedPath $seed
   Copy-Item $seed $cache -Recurse
  }
  New-Item -ItemType Directory -Force $cache | Out-Null
  Assert-OwnedPath $cache
  Write-Json (Cache-Manifest $cache) "$prefix.cache-start.json"
  Copy-Item $template $settings -Force
  Remove-Item $log -ErrorAction SilentlyContinue
  $exe = Join-Path $run.package 'SlipstreamViewer.exe'
  $identity = @{source=$run.source; executable_sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();
   capture_manifest_sha256=(Get-FileHash (Join-Path $run.capture 'manifest.json')).Hash.ToLowerInvariant();
   path_sha256=(Get-FileHash $run.path).Hash.ToLowerInvariant(); settings=$run.settings;
   settings_template_sha256=(Get-FileHash $template).Hash.ToLowerInvariant();
   app_cache_start=$(if($run.cold){'empty'}else{'retained'});
   driver_disk_cache_requested='unchanged; no purge'; worker_requested='AMD default'}
  Write-Json $identity "$prefix.identity.json"
  # The copied preferences may name an owner cache. Explicitly override both paths.
  $argsList = @('--bench',$run.capture,'--bench-path',$run.path,'--bench-loops','1',
    '--bench-report',"$prefix.json",'--set','CacheLocation',$cache,'--set','NewCacheLocation',$cache,
    '--set','SlipstreamSoundCacheLocation',"$cache\sounds")
  foreach ($prop in ($run.settings.PSObject.Properties | Sort-Object Name)) {
   $argsList += @('--set',$prop.Name,[string]$prop.Value)
  }
  # Campaign paths contain no quotes or trailing slash; quote every argument.
  if (@($argsList | Where-Object {$_ -match '"|\\$'}).Count) { throw 'unsupported argument quoting' }
  $quoted = ($argsList | ForEach-Object {'"'+$_+'"'}) -join ' '
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $mainPeak = [int64]0; $familyPeak = [int64]0; $workingPeak = [int64]0; $samples=0
  Note "launch $($run.name)"
  $proc = Start-Process -FilePath $exe -WorkingDirectory $run.package -ArgumentList $quoted -PassThru
  while (!$proc.HasExited) {
   Assert-Lock
   if ($watch.Elapsed.TotalSeconds -gt 1200 -or (Test-Path "$Root\STOP")) {
    # Kill only this launched viewer; its job object owns the compile worker.
    Stop-Process -Id $proc.Id -Force
    throw 'owned launch timed out or stopped'
   }
   try {
    $proc.Refresh()
    $mainPeak = [Math]::Max($mainPeak,[int64]$proc.PrivateMemorySize64)
    $workingPeak = [Math]::Max($workingPeak,[int64]$proc.PeakWorkingSet64)
    $sum=[int64]0
    # Same executable covers the viewer and compile-worker child, excluding media plugins.
    foreach ($other in (Get-Process -Name SlipstreamViewer -ErrorAction SilentlyContinue)) {
     if ($other.Path -eq $exe) { $sum += [int64]$other.PrivateMemorySize64 }
    }
    $familyPeak = [Math]::Max($familyPeak,$sum); $samples++
   } catch [System.InvalidOperationException] { }
   Start-Sleep -Milliseconds 250
  }
  $proc.WaitForExit(); $watch.Stop(); $proc.Refresh()
  Write-Json @{exit_code=$proc.ExitCode;wall_s=$watch.Elapsed.TotalSeconds;
   sampled_private_commit_peak_bytes=$mainPeak;sampled_viewer_worker_private_commit_peak_bytes=$familyPeak;
   peak_working_set_bytes=$workingPeak;sample_period_ms=250;samples=$samples} "$prefix.memory.json"
  if (Test-Path $log) { Copy-Item $log "$prefix.log" }
  if ($proc.ExitCode -ne 0 -or !(Test-Path "$prefix.json")) { throw 'viewer failed or report missing' }
  $report = Get-Content "$prefix.json" -Raw | ConvertFrom-Json
  if (!$report.ok -or !$report.pipelines.settled -or $report.overall.unfocused_frames -ne 0) { throw 'unsettled/unfocused/failed replay' }
  if ($report.pipelines.items_dropped -ne 0 -or $report.pipelines.batches_dropped -ne 0) { throw 'dropped pipeline telemetry' }
  foreach ($batch in $report.pipelines.batches) {
   if ($batch.failed -ne 0 -or $batch.at_s -gt $report.pipelines.timed_start_s) { throw 'pipeline failure or late compile' }
  }
  # A worker may finish after viewer shutdown: do not hash files while it writes.
  $deadline=[DateTime]::UtcNow.AddSeconds(30)
  while (Viewer-Running) {
   if ([DateTime]::UtcNow -gt $deadline) { throw 'viewer/worker remains after replay' }
   Start-Sleep -Milliseconds 250
  }
  Write-Json (Cache-Manifest $cache) "$prefix.cache-end.json"
  if ($run.save_seed) {
   $dest=Join-Path $Root $run.save_seed; Assert-OwnedPath $dest
   if (Test-Path $dest) { throw 'seed already exists' }
   Copy-Item $cache $dest -Recurse
  }
  Note "exit $($run.name) code=$($proc.ExitCode)"
 }
 'complete' | Set-Content "$Out\DONE"
} catch {
 # Retain reservation/backups on failure, but stop only our still-running launch.
 if ($proc -and !$proc.HasExited -and $proc.Path -eq $exe) { Stop-Process -InputObject $proc -Force }
 Note ('BLOCKED '+$_.Exception.Message)
 $_.Exception.Message | Set-Content "$Out\BLOCKED"
 exit 2
}

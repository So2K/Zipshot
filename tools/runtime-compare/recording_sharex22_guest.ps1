param(
 [string]$InputRoot='C:/ZipshotLab/input',
 [string]$OutputRoot='C:/ZipshotLab/output/recording22',
 [string]$WorkRoot='C:/ZipshotLab/work/recording22',
 [string]$PersonalRoot='C:/ZipshotLab/work/sharex22',
 [ValidateSet('video','gif')][string]$Mode='video',
 [ValidateSet('native-hardware','native-software-allowed')][string]$Profile='native-hardware',
 [int]$FPS=30,
 [ValidateRange(1,300)][int]$Seconds=30,
 [switch]$SkipSetupBar,
 [ValidateSet('mapped','local')][string]$ExecutableLocation='local'
)
$ErrorActionPreference='Stop'
if($Mode -eq 'gif' -and -not $PSBoundParameters.ContainsKey('FPS')){$FPS=15}
if($env:USERNAME -ne 'WDAGUtilityAccount'){throw 'This adapter only runs inside the clean Windows Sandbox guest.'}
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot);$WorkRoot=[IO.Path]::GetFullPath($WorkRoot);$PersonalRoot=[IO.Path]::GetFullPath($PersonalRoot)
if(-not $OutputRoot.StartsWith('C:\ZipshotLab\output\',[StringComparison]::OrdinalIgnoreCase)){throw 'Output must use the task-owned sandbox output mapping.'}
foreach($path in @($WorkRoot,$PersonalRoot)){if(-not $path.StartsWith('C:\ZipshotLab\work\',[StringComparison]::OrdinalIgnoreCase)){throw 'All working settings and media must stay in the clean guest work folder.'}}
$exe=Join-Path $InputRoot 'sharex22/ShareX.exe';$dll=Join-Path $InputRoot 'sharex22/ShareX.dll'
if(-not(Test-Path -LiteralPath $exe)){throw 'The read-only official ShareX 22 package is missing.'}
$existing=@(Get-Process -Name ShareX -ErrorAction SilentlyContinue)
if($existing.Count){throw 'Another ShareX job is still running. The root orchestrator must finish it before this serialized full-UI test.'}
$caseName='sharex22-'+$Mode+$FPS+'-'+$Profile
$caseOutput=Join-Path $OutputRoot $caseName
if(Test-Path -LiteralPath $caseOutput){throw 'Completed case already exists. Choose a fresh OutputRoot.'}
$local=Join-Path $WorkRoot ('run-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
$caseRoot=Join-Path $local $caseName
foreach($path in @($OutputRoot,$local,$caseRoot,$PersonalRoot)){New-Item -ItemType Directory -Path $path -Force | Out-Null}
if($ExecutableLocation -eq 'local'){
 $localInfo=& (Join-Path $InputRoot 'recording/recording_local_prepare.ps1') -InputRoot $InputRoot -NeedShareX22
 $exe=Join-Path $localInfo.sharex22 'ShareX.exe';$dll=Join-Path $localInfo.sharex22 'ShareX.dll'
 $localInfo | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $caseRoot 'recording_local_setup.json') -Encoding utf8
}

# Fresh task-owned settings. These are supported product preferences, never installed user settings.
# Json.NET serializes System.Drawing.Rectangle as this converter string.
$config=[ordered]@{
 ShowStartScreen=$false;ShowIconPackPrompt=$false;FirstTimeMinimizeToTray=$false;AutoCheckUpdate=$false;SilentRun=$true;ShowTray=$true;DisableUpload=$true;ShowClipboardContentViewer=$false;Language='English'
 DefaultTaskSettings=[ordered]@{
  UseDefaultAfterCaptureJob=$false;AfterCaptureJob=0;UseDefaultAfterUploadJob=$false;AfterUploadJob=0
  OverrideScreenshotsFolder=$true;ScreenshotsFolder=$caseRoot
  UseDefaultGeneralSettings=$false;GeneralSettings=@{PlaySoundAfterCapture=$false;PlaySoundAfterUpload=$false;PlaySoundAfterAction=$false;ShowToastNotificationAfterTaskCompleted=$false}
  UseDefaultCaptureSettings=$false;CaptureSettings=[ordered]@{
   CaptureCustomRegion='0, 0, 1920, 1080';ScreenRecordUseNative=$true;ScreenRecordShowBar=(-not $SkipSetupBar)
   ScreenRecordSystemAudio=$false;ScreenRecordMicrophone=$false;ScreenRecordCamera=$false
   ScreenRecordRequireHardwareEncoder=($Profile -eq 'native-hardware');ScreenRecordVideoBitrate=10000
   ScreenRecordFPS=$FPS;GIFFPS=$FPS;ScreenRecordShowCursor=$false;ScreenRecordStartDelayEnabled=$false
   ScreenRecordFixedDuration=$true;ScreenRecordDuration=$Seconds
  }
 }
}
$configPath=Join-Path $PersonalRoot 'ApplicationConfig.json'
$priorConfigBytes=if(Test-Path -LiteralPath $configPath){[IO.File]::ReadAllBytes($configPath)}else{$null}
$config | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $configPath -Encoding utf8
Copy-Item -LiteralPath $configPath -Destination (Join-Path $caseRoot 'recording_application_config.json')

# Reuse the reviewed sampler source. Loading this class does not start recording or UI.
$samplerPath=Join-Path $InputRoot 'recording/recording_guest_legacy.ps1'
if(-not(Test-Path -LiteralPath $samplerPath)){$samplerPath=Join-Path $InputRoot 'recording/recording_guest.ps1'}
$samplerText=Get-Content -LiteralPath $samplerPath -Raw
$blocks=[regex]::Matches($samplerText,"(?s)@'\r?\n(.*?)\r?\n'@")
if($blocks.Count -lt 2){throw 'The reviewed recording sampler definition was not found.'}
Add-Type -TypeDefinition $blocks[$blocks.Count-1].Groups[1].Value
$fixtureStart=[Diagnostics.ProcessStartInfo]::new((Join-Path $InputRoot 'recording/recording_fixture.exe'),('"'+$local+'"'))
$fixtureStart.UseShellExecute=$false;$fixtureStart.CreateNoWindow=$true
$fixture=[Diagnostics.Process]::Start($fixtureStart)
Start-Sleep -Seconds 2

function Invoke-ProductCLI([string]$Command){
 $info=[Diagnostics.ProcessStartInfo]::new($exe,$Command)
 $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.WorkingDirectory=Split-Path $exe -Parent
 $cli=[Diagnostics.Process]::Start($info)
 if(-not $cli.WaitForExit(15000)){throw ('The owned ShareX control command timed out '+$Command)}
 return $cli.ExitCode
}

$gpuPath=Join-Path $caseRoot 'recording_gpu_samples.jsonl'
$gpuJob=Start-Job -ArgumentList $gpuPath,($Seconds+120) -ScriptBlock {
 param($path,$max)
 try{Get-Counter -Counter '\GPU Engine(*)\Utilization Percentage' -SampleInterval 1 -MaxSamples $max -ErrorAction Stop | ForEach-Object {
  $batch=@($_.CounterSamples | ForEach-Object {@{instance=$_.InstanceName;percent=$_.CookedValue}})
  @{utc=$_.Timestamp.ToUniversalTime().ToString('o');engines=$batch} | ConvertTo-Json -Depth 4 -Compress | Add-Content -LiteralPath $path -Encoding utf8
 }}catch{@{unavailable=$_.Exception.GetType().Name} | ConvertTo-Json -Compress | Set-Content -LiteralPath $path -Encoding utf8}
}
$startCommand=if($Mode -eq 'gif'){'-ScreenRecorderGIFCustomRegion'}else{'-ScreenRecorderCustomRegion'}
$arguments='-silent -NoHotkeys '+$startCommand
$start=[Diagnostics.ProcessStartInfo]::new($exe,$arguments);$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.WorkingDirectory=Split-Path $exe -Parent
$clock=[Diagnostics.Stopwatch]::StartNew();$utc=[DateTime]::UtcNow
$process=$null;$metrics=$null;$timedOut=$false;$setupTriggered=[bool]$SkipSetupBar;$stopRequested=$false;$nativeStart=$null;$completed=$null;$failure=$null;$cleanupNote=$null;$nativeMetadata=$null
$samples=[Collections.Generic.List[object]]::new();$childCommands=@();$seen=@{};$lastLogCheck=-1.0;$stablePath=$null;$stableSince=$null
$logOffsets=@{};$productLog='';$failureObserved=$null
foreach($oldLog in @(Get-ChildItem -LiteralPath (Join-Path $PersonalRoot 'Logs') -File -ErrorAction SilentlyContinue)){$logOffsets[$oldLog.FullName]=$oldLog.Length}
try{
 $process=[Diagnostics.Process]::Start($start);$metrics=[RecordingProcessMetrics]::new($process.Id)
 while($true){
  $rows=@($metrics.Sample());$t=$clock.Elapsed.TotalSeconds
  $samples.Add([ordered]@{elapsed_seconds=$t;processes=$rows})
  foreach($row in $rows){if($row.name -eq 'ffmpeg.exe' -and -not $seen.ContainsKey($row.pid)){
   $seen[$row.pid]=$true;$child=Get-CimInstance Win32_Process -Filter ('ProcessId='+$row.pid) -ErrorAction SilentlyContinue
   $childCommands+=@{pid=$row.pid;first_seen_seconds=$t;command=$child.CommandLine}
  }}
  if($t-$lastLogCheck -ge .5){
   $lastLogCheck=$t
   $logFiles=@(Get-ChildItem -LiteralPath (Join-Path $PersonalRoot 'Logs') -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1)
   if($logFiles.Count){
    $logPath=$logFiles[0].FullName;$offset=if($logOffsets.ContainsKey($logPath)){$logOffsets[$logPath]}else{0}
    $stream=[IO.File]::Open($logPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try{$stream.Seek($offset,[IO.SeekOrigin]::Begin) | Out-Null;$reader=[IO.StreamReader]::new($stream);$appended=$reader.ReadToEnd();$logOffsets[$logPath]=$stream.Position;$productLog+=$appended}finally{$stream.Close()}
    $tail=$productLog
    if($null -eq $nativeStart -and $tail -match 'Starting native screen recording'){$nativeStart=$t}
    if($tail -match 'Native screen recording completed\. Encoder: (.*), hardware: (True|False), frames: (\d+), dropped: (\d+), audio discontinuities: (\d+)'){
     $nativeMetadata=@{encoder=$matches[1];hardware_accelerated=($matches[2] -eq 'True');video_frames=[int64]$matches[3];dropped_video_frames=[int64]$matches[4];audio_discontinuities=[int64]$matches[5]}
    }
    if(-not $failure -and $tail -match 'does not provide D3D11 hardware video encoding|Media Foundation did not expose the selected video encoder|Windows did not deliver a frame'){$failure=$matches[0];$failureObserved=$t}
   }
  }
  if(-not $setupTriggered -and $null -ne $nativeStart -and $t -ge $nativeStart+1.0){
   # Supported product hotkey/CLI action requests Record on the existing setup bar.
   $code=Invoke-ProductCLI '-StartScreenRecorder';$setupTriggered=$true
   @{elapsed_seconds=$clock.Elapsed.TotalSeconds;command='-StartScreenRecorder';exit_code=$code} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $caseRoot 'recording_start_control.json') -Encoding utf8
  }
  $finalFiles=@(Get-ChildItem -LiteralPath $caseRoot -Recurse -File | Where-Object {$_.Extension -in @('.mp4','.gif') -and $_.BaseName -notmatch '-recording-'})
  if($finalFiles.Count -and $null -eq $completed){
   $candidate=$finalFiles[0].FullName
   try{$read=[IO.File]::Open($candidate,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None);$read.Close()
    if($stablePath -ne $candidate){$stablePath=$candidate;$stableSince=$t}
    elseif($t-$stableSince -ge 1.0){$completed=$t}
   }catch{}
  }
  if($null -ne $completed -and -not $stopRequested){Invoke-ProductCLI '-ExitShareX' | Out-Null;$stopRequested=$true}
  if($failure -and -not $stopRequested){Invoke-ProductCLI '-ExitShareX' | Out-Null;$stopRequested=$true}
  if($null -ne $failureObserved -and $t-$failureObserved -gt 10){break}
  if($process.HasExited -and -not($rows | Where-Object alive)){break}
  if($t -gt $Seconds+90){$timedOut=$true;$failure='No completed file before the bounded deadline';break}
  Start-Sleep -Milliseconds 200
 }
 $wall=$clock.Elapsed.TotalSeconds
 if($timedOut -and -not $process.HasExited){try{Invoke-ProductCLI '-ExitShareX' | Out-Null}catch{}}
 if(-not $process.HasExited -and -not $process.WaitForExit(5000)){$cleanupNote='Owned ShareX did not exit after its supported ExitShareX command; the failed isolated test process was stopped.';if(-not $failure){$failure=$cleanupNote};foreach($row in @($metrics.Sample()) | Where-Object alive){Stop-Process -Id $row.pid -Force -ErrorAction SilentlyContinue}}
 $final=@($metrics.Sample());$metrics.Close();$metrics=$null
 Stop-Job -Job $gpuJob -ErrorAction SilentlyContinue;Remove-Job -Job $gpuJob -Force -ErrorAction SilentlyContinue;$gpuJob=$null
 $samples | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $caseRoot 'recording_samples.json') -Encoding utf8
 $childCommands | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $caseRoot 'recording_ffmpeg_commands.json') -Encoding utf8
 $files=@();$ffprobe=Join-Path $InputRoot 'ffprobe.exe'
 if(-not(Test-Path -LiteralPath $ffprobe)){$ffprobe=Join-Path $InputRoot 'recording/ffprobe.exe'}
 foreach($file in @(Get-ChildItem -LiteralPath $caseRoot -Recurse -File | Where-Object {$_.Extension -in @('.mp4','.gif')})){
  $priorErrorAction=$ErrorActionPreference
  try{$ErrorActionPreference='Continue';$raw=& $ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=codec_name,width,height,avg_frame_rate,r_frame_rate,nb_read_frames,duration,pix_fmt,bit_rate -show_entries format=duration,size,bit_rate -of json $file.FullName 2>&1;$probeExit=$LASTEXITCODE}finally{$ErrorActionPreference=$priorErrorAction}
  $parsed=try{($raw -join "`n") | ConvertFrom-Json}catch{$null}
  $files+=@{relative_path=$caseName+'\'+$file.FullName.Substring($caseRoot.Length+1);bytes=$file.Length;ffprobe_exit=$probeExit;ffprobe=$parsed;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
 }
 if(Test-Path -LiteralPath (Join-Path $PersonalRoot 'Logs')){Copy-Item -LiteralPath (Join-Path $PersonalRoot 'Logs') -Destination (Join-Path $caseRoot 'product-logs') -Recurse}
 $productLog | Set-Content -LiteralPath (Join-Path $caseRoot 'recording_product_log.txt') -Encoding utf8
 if(-not $files.Count -and -not $failure){$failure='The stock product exited without a media file.'}
 $case=[ordered]@{name=$caseName;product='ShareX';version='22.4.0';stock_executable=$true;assembly_sha256=(Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant();profile=$Profile;configuration_bar_shown=(-not $SkipSetupBar);audio=$false;cursor=$false;requested_seconds=$Seconds;fps=$FPS;width=1920;height=1080;video_bitrate_kbps=10000;start_utc=$utc.ToString('o');wall_seconds=$wall;timed_out=$timedOut;failure=$failure;cleanup_note=$cleanupNote;parent_pid=$process.Id;final_processes=$final;files=$files;ffmpeg_commands=$childCommands;native_start_observed_seconds=$nativeStart;completed_file_observed_seconds=$completed;native_result=$nativeMetadata;frame_drop_counter=if($nativeMetadata){$nativeMetadata.dropped_video_frames}else{$null};control_overhead_note='The setup-bar Record action and application Exit use supported product CLI messages. Those separate automation-trigger processes are excluded from the measured recording process tree.'}
 $report=[ordered]@{schema_version=1;scope='Official unmodified ShareX 22.4.0 full GUI, actual default native recorder, controlled through supported CLI tasks in a clean Windows Sandbox.';io_note='Hot media and settings are guest-local; completed files are copied to the host task mapping after process sampling.';executable_location=$ExecutableLocation;logical_processors=[Environment]::ProcessorCount;sample_interval_ms=200;cases=@($case)}
 Copy-Item -LiteralPath $caseRoot -Destination $caseOutput -Recurse
 $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $OutputRoot 'recording_guest_results.json') -Encoding utf8
 Write-Output ($caseName+' completed, files '+$files.Count+', timeout '+$timedOut)
} finally {
 if($process -and -not $process.HasExited){
  if($metrics){foreach($row in @($metrics.Sample()) | Where-Object alive){Stop-Process -Id $row.pid -Force -ErrorAction SilentlyContinue}}
  if(-not $process.HasExited){Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue}
 }
 if($metrics){$metrics.Close()}
 if($gpuJob){Stop-Job -Job $gpuJob -ErrorAction SilentlyContinue;Remove-Job -Job $gpuJob -Force -ErrorAction SilentlyContinue}
 if($fixture -and -not $fixture.HasExited){Stop-Process -Id $fixture.Id -Force -ErrorAction SilentlyContinue}
 if($process -and -not $process.HasExited){$process.WaitForExit(5000) | Out-Null}
 if($priorConfigBytes){[IO.File]::WriteAllBytes($configPath,$priorConfigBytes)}
}

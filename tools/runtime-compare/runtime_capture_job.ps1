param([ValidateSet('zipshot','sharex18','sharex22')][string]$App='zipshot',[ValidateSet('region','full')][string]$Scenario='region',[int]$Count=1,[int]$Warmups=0,[string]$Name='capture-smoke',[switch]$Record,[string]$ApiDiagnostic='',[switch]$TraceDiagnostic,[ValidateSet('center','toolbar')][string]$Profile='center',[switch]$FullVideo)
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount' -or -not(Test-Path 'C:/ZipshotLab/input/.isolation-approved.json')){throw 'Guest only'}
if($Name -notmatch '^[a-zA-Z0-9_-]+$'){throw 'Invalid run name'}
$inputRoot='C:/ZipshotLab/input';$workRoot='C:/ZipshotLab/work';$outputRoot='C:/ZipshotLab/output'
$out=Join-Path $outputRoot $Name;$local=Join-Path $workRoot $Name
New-Item -ItemType Directory -Path $out,$local -Force|Out-Null
if($ApiDiagnostic) {
 if($ApiDiagnostic -notin @('static','moving')){throw 'Unknown API diagnostic mode'}
 $kind=if($TraceDiagnostic){'trace'}else{'plain'}
 $p=New-Object Diagnostics.ProcessStartInfo;$p.FileName="$inputRoot/capture_api_$kind.exe"
 $p.Arguments=('{0} {1} {2} "{3}"' -f $ApiDiagnostic,$Count,$Warmups,(Join-Path $out 'capture-api'))
 $p.UseShellExecute=$false;$p.CreateNoWindow=$true
 $child=[Diagnostics.Process]::Start($p)
 if(-not $child.WaitForExit(60000)){$child.Kill();throw 'API diagnostic exceeded one minute'}
 Copy-Item -LiteralPath "$inputRoot/capture_api_manifest.json" -Destination (Join-Path $out 'capture-api-source.json')
 if($child.ExitCode -ne 0){throw "API diagnostic failed: $($child.ExitCode)"}
 Get-Content -LiteralPath (Join-Path $out 'capture-api.json') -Raw
 return
}
$save=Join-Path $workRoot "shots/$App";New-Item -ItemType Directory -Path $save -Force|Out-Null
# Recording jobs may change this task-owned profile. Restore the capture workload
# before EVERY application launch; never import an installed host configuration.
if($App -ne 'zipshot') {
 $personal=Join-Path $workRoot $App
 New-Item -ItemType Directory -Path $personal -Force|Out-Null
 $config=[ordered]@{ShowStartScreen=$false;ShowIconPackPrompt=$false;FirstTimeMinimizeToTray=$false;AutoCheckUpdate=$false;ShowTray=$true;ShowClipboardContentViewer=$false;DisableUpload=$true;DisableHotkeys=$false;UseCustomScreenshotsPath=$true;CustomScreenshotsPath=$save;SaveImageSubFolderPattern='';Language='English';DefaultTaskSettings=@{AfterCaptureJob='CopyImageToClipboard, SaveImageToFile';AfterUploadJob='None';ImageSettings=@{ImageFormat='PNG';ImageAutoUseJPEG=$false};CaptureSettings=@{ShowCursor=$false};GeneralSettings=@{PlaySoundAfterCapture=$false;PlaySoundAfterUpload=$false;PlaySoundAfterAction=$false;ShowToastNotificationAfterTaskCompleted=$true;ToastWindowDuration=0.6;ToastWindowFadeDuration=0}}}
 $config|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $personal 'ApplicationConfig.json') -Encoding UTF8
}
$exe=if($App -eq 'zipshot'){Join-Path $inputRoot 'zipshot/zipshot.exe'}else{Join-Path $inputRoot "$App/ShareX.exe"}
$psi=New-Object Diagnostics.ProcessStartInfo
$psi.FileName=$exe;$psi.WorkingDirectory=Split-Path $exe -Parent;$psi.Arguments=if($App -eq 'zipshot'){''}else{'-silent -multi'}
$psi.UseShellExecute=$false
$process=[Diagnostics.Process]::Start($psi)
$observer=$null
try {
 Start-Sleep -Seconds 4
 if($process.HasExited){throw "App exited during startup ($($process.ExitCode))"}
 & "$inputRoot/capture_inspect.ps1" -OutputPrefix (Join-Path $out 'startup')
 $setup=Get-Content (Join-Path $out 'startup.json') -Raw|ConvertFrom-Json
 Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;public static class RuntimeCaptureDisplay{[DllImport("user32.dll")]public static extern int GetSystemMetrics(int i);[DllImport("user32.dll")]public static extern bool SetProcessDpiAwarenessContext(IntPtr c);[DllImport("user32.dll")]public static extern bool SetCursorPos(int x,int y);}
'@
 [RuntimeCaptureDisplay]::SetProcessDpiAwarenessContext([IntPtr](-4))|Out-Null
 $width=[RuntimeCaptureDisplay]::GetSystemMetrics(0);$height=[RuntimeCaptureDisplay]::GetSystemMetrics(1)
 $roiW=[Math]::Min(1280,$width);$roiH=[Math]::Min(720,$height)
 $x=if($Scenario -eq 'region'){[int](($width-$roiW)/2)}else{$width-$roiW}
 $y=if($Scenario -eq 'region'){[int](($height-$roiH)/2)}else{$height-$roiH}
 $measureX=$x;$measureY=$y;$measureW=$roiW;$measureH=$roiH
 if($Profile -eq 'toolbar'){
  if($Scenario -ne 'region'){throw 'The top-toolbar profile applies only to region capture.'}
  $measureX=[int]($width/2)-32;$measureY=0;$measureW=64;$measureH=64
 }
 [RuntimeCaptureDisplay]::SetCursorPos([int]($width/2),[int]($height/2))|Out-Null
 $video=Join-Path $local 'observer.mkv'
 $videoX=$x;$videoY=$y;$videoW=$roiW;$videoH=$roiH;$videoFps=60;$videoFilter=''
 if($FullVideo){$videoX=0;$videoY=0;$videoW=$width;$videoH=$height;$videoFps=30;$videoFilter='-vf scale=1920:-2:flags=fast_bilinear'}
 if($Record){
  $recordStart=[DateTime]::UtcNow.ToString('o')
  $r=New-Object Diagnostics.ProcessStartInfo;$r.FileName="$inputRoot/ffmpeg.exe"
  $r.Arguments="-hide_banner -loglevel info -y -probesize 32 -analyzeduration 0 -use_wallclock_as_timestamps 1 -f gdigrab -framerate $videoFps -draw_mouse 1 -offset_x $videoX -offset_y $videoY -video_size $($videoW)x$($videoH) -i desktop -copyts -start_at_zero -an $videoFilter -c:v libx264 -preset ultrafast -crf 12 -threads 2 -pix_fmt yuv420p -fps_mode passthrough $video"
  $r.UseShellExecute=$false;$r.CreateNoWindow=$true;$r.RedirectStandardInput=$true;$r.RedirectStandardError=$true
  $observer=[Diagnostics.Process]::Start($r);$observerError=$observer.StandardError.ReadToEndAsync()
  Start-Sleep -Milliseconds 800
  if($observer.HasExited){throw 'Observer video recorder failed before input'}
 }
 $result=Join-Path $out 'capture-results.json'
 & "$inputRoot/capture_hotkeys_guest.ps1" -TargetPid $process.Id -Scenario $Scenario -Count $Count -Warmups $Warmups -X $measureX -Y $measureY -Width $measureW -Height $measureH -SaveDir $save -ResultPath $result -Controller "$inputRoot/capture_hotkeys.exe" -SaveEndpointFrame:$Record
 & "$inputRoot/capture_inspect.ps1" -OutputPrefix (Join-Path $out 'finished')
 $metadata=[ordered]@{app=$App;scenario=$Scenario;profile=$Profile;app_exe=$exe;app_sha256=(Get-FileHash $exe -Algorithm SHA256).Hash;controller_sha256=(Get-FileHash "$inputRoot/capture_hotkeys.exe" -Algorithm SHA256).Hash;display=@{width=$width;height=$height};roi=@{x=$measureX;y=$measureY;width=$measureW;height=$measureH};video_roi=@{x=$videoX;y=$videoY;width=$videoW;height=$videoH};video_full_desktop=[bool]$FullVideo;video_requested_fps=$videoFps;observer_enabled=[bool]$Record;version=(Get-Item $exe).VersionInfo.ProductVersion;save_dir=$save;output_endpoint_note='Synthetic SendInput keyboard events in isolated guest, not physical hardware input. Pixel, valid clipboard image and closed decoded image file are separate endpoints.'}
 if($Record -and $Scenario -eq 'full') {
  $raw=Get-Content -LiteralPath $result -Raw|ConvertFrom-Json
  $saved=[Collections.Generic.List[object]]::new()
  foreach($trial in $raw.trials){
   if($trial.file_path -and (Test-Path -LiteralPath $trial.file_path)){
    $copy=Join-Path $out ('capture-saved-'+$trial.index+[IO.Path]::GetExtension($trial.file_path))
    Copy-Item -LiteralPath $trial.file_path -Destination $copy
    $saved.Add([pscustomobject]@{trial=$trial.index;file=[IO.Path]::GetFileName($copy);sha256=(Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash})
   }
  }
  $metadata.actual_saved_image_copies=@($saved)
 }
 if($observer){
  $observer.StandardInput.WriteLine('q');$observer.StandardInput.Close()
  if(-not $observer.WaitForExit(30000)){$observer.Kill();throw 'Observer failed to finish'}
  $observerError.Result|Set-Content (Join-Path $out 'observer-ffmpeg.log') -Encoding UTF8
  $startMatch=[regex]::Match($observerError.Result,'Duration: N/A, start: ([0-9]+\.[0-9]+)')
  if($startMatch.Success){$metadata.observer_source_start_unix_seconds=[double]::Parse($startMatch.Groups[1].Value,[Globalization.CultureInfo]::InvariantCulture)}
  $probe=& "$inputRoot/ffprobe.exe" -v error -count_frames -show_streams -show_format -of json $video
  if($LASTEXITCODE -ne 0){throw 'Observer ffprobe failed'}
  $metadata.observer_start_utc=$recordStart;$metadata.observer_probe=$probe|ConvertFrom-Json
  $metadata.observer_time_policy='Original inter-frame timestamps retained; only the timestamp origin is shifted to zero. No speed change, frame interpolation or output fps enforcement.'
  Copy-Item -LiteralPath $video -Destination (Join-Path $out 'observer.mkv')
  $packets=& "$inputRoot/ffprobe.exe" -v error -select_streams v:0 -show_entries packet=pts_time,duration_time -of csv=p=0 $video
  $packets|Set-Content (Join-Path $out 'observer-frame-times.csv') -Encoding UTF8
 }
 $metadata|ConvertTo-Json -Depth 10|Set-Content (Join-Path $out 'metadata.json') -Encoding UTF8
 Write-Output ($App+' '+$Scenario+' completed')
} finally {
 if($observer -and -not $observer.HasExited){$observer.StandardInput.WriteLine('q');$observer.WaitForExit(10000)|Out-Null}
 if(-not $process.HasExited){$process.CloseMainWindow()|Out-Null;if(-not $process.WaitForExit(1000)){$process.Kill();$process.WaitForExit()}}
 if($App -eq 'zipshot' -and (Test-Path -LiteralPath "$env:TEMP/zipshot.log")){
  Copy-Item -LiteralPath "$env:TEMP/zipshot.log" -Destination (Join-Path $out 'capture-zipshot.log') -Force
 }
}

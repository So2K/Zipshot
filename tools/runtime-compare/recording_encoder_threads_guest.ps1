param([string]$InputRoot='C:/ZipshotLab/input/recording',[string]$OutputRoot='C:/ZipshotLab/output/recording-encoder-threads')
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount'){throw 'Clean guest only'}
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
if(-not $OutputRoot.StartsWith('C:\ZipshotLab\output\')){throw 'Task output mapping required'}
$localInfo=& (Join-Path $InputRoot 'recording_local_prepare.ps1') -InputRoot (Split-Path $InputRoot -Parent)
$samplerText=Get-Content -LiteralPath (Join-Path $InputRoot 'recording_guest_legacy.ps1') -Raw
$blocks=[regex]::Matches($samplerText,"(?s)@'\r?\n(.*?)\r?\n'@")
Add-Type -TypeDefinition $blocks[$blocks.Count-1].Groups[1].Value
$work='C:/ZipshotLab/work/recording-encoder-threads'
New-Item -ItemType Directory -Path $work,$OutputRoot -Force | Out-Null
$results=@()
foreach($threads in @(0,4,4,0)){
 $index=$results.Count;$progress=Join-Path $work ('progress-'+$index+'.txt')
 $arguments='-hide_banner -loglevel warning -y -re -f lavfi -i testsrc2=size=1920x1080:rate=30 -frames:v 90 -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p -threads '+$threads+' -progress "'+$progress+'" -f null NUL'
 $info=[Diagnostics.ProcessStartInfo]::new($localInfo.ffmpeg,$arguments);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
 $clock=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($info);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
 $metrics=[RecordingProcessMetrics]::new($process.Id);$samples=@();$timedOut=$false
 try{
  while(-not $process.HasExited){$samples+=@{elapsed_seconds=$clock.Elapsed.TotalSeconds;processes=@($metrics.Sample())};if($clock.Elapsed.TotalSeconds -gt 30){$timedOut=$true;$process.Kill();break};Start-Sleep -Milliseconds 50}
  $process.WaitForExit();$final=@($metrics.Sample());$peakPrivate=0L;$peakWS=0L
  foreach($sample in $samples){$active=@($sample.processes | Where-Object alive);$p=[int64]($active | Measure-Object private_bytes -Sum).Sum;$w=[int64]($active | Measure-Object working_set_bytes -Sum).Sum;if($p -gt $peakPrivate){$peakPrivate=$p};if($w -gt $peakWS){$peakWS=$w}}
  $progressText=if(Test-Path -LiteralPath $progress){Get-Content -LiteralPath $progress -Raw}else{''}
  $frames=@([regex]::Matches($progressText,'(?m)^frame=(\d+)'));$frameCount=if($frames.Count){[int]$frames[$frames.Count-1].Groups[1].Value}else{$null}
  $results+=@{threads_option=$threads;scope='Synthetic encoder only, not product capture';input='testsrc2 1920x1080 30fps 90frames realtime';encoder='libx264 veryfast CRF23 yuv420p';wall_seconds=$clock.Elapsed.TotalSeconds;cpu_seconds=($final | Measure-Object cpu_seconds -Sum).Sum;peak_private_bytes=$peakPrivate;peak_working_set_bytes=$peakWS;frames=$frameCount;timed_out=$timedOut;exit_code=$process.ExitCode;error=$stderr.Result}
  $samples | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputRoot ('recording_thread_samples-'+$index+'.json')) -Encoding utf8
 }finally{if(-not $process.HasExited){$process.Kill()};$metrics.Close();$stdout.Result | Out-Null}
}
@{schema_version=1;scope='Synthetic libx264 thread-budget diagnostic. This is not an application recording benchmark or evidence of equal perceptual quality.';ffmpeg_sha256=$localInfo.ffmpeg_sha256;runs=$results} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputRoot 'recording_encoder_threads.json') -Encoding utf8
Write-Output 'Completed the synthetic encoder thread-budget control.'

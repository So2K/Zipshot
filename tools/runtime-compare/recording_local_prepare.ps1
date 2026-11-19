param([string]$InputRoot='C:/ZipshotLab/input',[switch]$NeedShareX22)
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount'){throw 'Clean guest only'}
$tools='C:/ZipshotLab/work/tools';$app='C:/ZipshotLab/work/recording-tools/sharex22'
New-Item -ItemType Directory -Path $tools -Force | Out-Null
$ffmpeg=Join-Path $tools 'ffmpeg.exe';$source=Join-Path $InputRoot 'recording/ffmpeg.exe'
$sourceHash=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$package=Join-Path $InputRoot 'sharex22';$copyApp=$NeedShareX22 -and -not(Test-Path -LiteralPath (Join-Path $app '.recording-local-copy.json'))
$needed=0L;if(-not(Test-Path -LiteralPath $ffmpeg)){$needed+=(Get-Item -LiteralPath $source).Length}
if($copyApp){$needed+=[int64](Get-ChildItem -LiteralPath $package -Recurse -File | Measure-Object Length -Sum).Sum}
if($needed -gt 0){
 $spacePath=Join-Path $InputRoot 'recording/recording_local_space.json'
 if(-not(Test-Path -LiteralPath $spacePath)){throw 'A current host free-space measurement is required before staging guest-local images.'}
 $space=Get-Content -LiteralPath $spacePath -Raw | ConvertFrom-Json
 if(([DateTime]::UtcNow-[DateTime]::Parse($space.utc).ToUniversalTime()).TotalMinutes -gt 15){throw 'Host free-space measurement is stale; refresh it before local package staging.'}
 if([int64]$space.host_c_free_bytes-$needed -lt 1288490188){throw 'The measured host C drive cannot retain the required 1.2GiB reserve after this local copy.'}
}
if(-not(Test-Path -LiteralPath $ffmpeg)){
 $diagnostic='C:/ZipshotLab/work/recording-image-io/ffmpeg.exe'
 if(Test-Path -LiteralPath $diagnostic){try{New-Item -ItemType HardLink -Path $ffmpeg -Value $diagnostic -ErrorAction Stop | Out-Null}catch{Copy-Item -LiteralPath $source -Destination $ffmpeg}}
 else{Copy-Item -LiteralPath $source -Destination $ffmpeg}
}
if((Get-FileHash -LiteralPath $ffmpeg -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sourceHash){throw 'Guest-local FFmpeg differs from the shared input.'}
$warmMarker=Join-Path $tools '.ffmpeg-image-warmed.sha256'
if(-not(Test-Path -LiteralPath $warmMarker)){
 $info=[Diagnostics.ProcessStartInfo]::new($ffmpeg,'-version');$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
 $warm=[Diagnostics.Process]::Start($info);$stdout=$warm.StandardOutput.ReadToEndAsync();$stderr=$warm.StandardError.ReadToEndAsync()
 if(-not $warm.WaitForExit(20000)){$warm.Kill();throw 'Local FFmpeg initialization did not finish'}
 if($warm.ExitCode -ne 0){throw 'Local FFmpeg initialization failed'}
 $stdout.Result | Out-Null;$stderr.Result | Out-Null;$sourceHash | Set-Content -LiteralPath $warmMarker -Encoding ascii
}
if($copyApp){
 New-Item -ItemType Directory -Path (Split-Path $app -Parent) -Force | Out-Null
 Copy-Item -LiteralPath $package -Destination $app -Recurse
 foreach($name in @('ShareX.exe','ShareX.dll','ShareX.runtimeconfig.json','ShareX.deps.json')){
  if((Get-FileHash -LiteralPath (Join-Path $package $name) -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath (Join-Path $app $name) -Algorithm SHA256).Hash){throw ('Local ShareX identity mismatch '+$name)}
 }
 @{sha256=(Get-FileHash -LiteralPath (Join-Path $app 'ShareX.dll') -Algorithm SHA256).Hash.ToLowerInvariant();source='Official unchanged 22.4.0 package; task copy';utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $app '.recording-local-copy.json') -Encoding utf8
}
[pscustomobject]@{ffmpeg=$ffmpeg;sharex22=if($NeedShareX22){$app}else{$null};ffmpeg_sha256=$sourceHash;ffmpeg_image='guest-local, initialized outside measured product lifecycle';copy_bytes_upper_bound=$needed}

param([string]$Source='C:/ZipshotLab/input/recording/ffmpeg.exe',[string]$OutputRoot='C:/ZipshotLab/output/recording-image-io',[string]$WorkRoot='C:/ZipshotLab/work/recording-image-io')
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount'){throw 'Clean guest only'}
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot);$WorkRoot=[IO.Path]::GetFullPath($WorkRoot)
if(-not $OutputRoot.StartsWith('C:\ZipshotLab\output\') -or -not $WorkRoot.StartsWith('C:\ZipshotLab\work\')){throw 'Task-owned guest paths required'}
New-Item -ItemType Directory -Path $OutputRoot,$WorkRoot -Force | Out-Null
$local=Join-Path $WorkRoot 'ffmpeg.exe'
if(-not(Test-Path -LiteralPath $local)){Copy-Item -LiteralPath $Source -Destination $local}
$sourceHash=(Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
if((Get-FileHash -LiteralPath $local -Algorithm SHA256).Hash -ne $sourceHash){throw 'Local copy differs from source'}
$controller=[Diagnostics.Process]::GetCurrentProcess();$runs=@()
foreach($location in @('mapped','local','local','mapped','mapped','local')){
 $file=if($location -eq 'mapped'){$Source}else{$local}
 $info=[Diagnostics.ProcessStartInfo]::new($file,'-version');$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
 $controller.Refresh();$beforeKernel=$controller.PrivilegedProcessorTime.TotalSeconds;$beforeUser=$controller.UserProcessorTime.TotalSeconds
 $clock=[Diagnostics.Stopwatch]::StartNew();$child=[Diagnostics.Process]::Start($info);$creationWall=$clock.Elapsed.TotalSeconds
 $controller.Refresh();$creationKernel=$controller.PrivilegedProcessorTime.TotalSeconds-$beforeKernel;$creationUser=$controller.UserProcessorTime.TotalSeconds-$beforeUser
 $stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync()
 $timeout=-not $child.WaitForExit(20000);if($timeout){$child.Kill();$child.WaitForExit()}
 $runs+=@{location=$location;create_wall_seconds=$creationWall;create_parent_kernel_seconds=$creationKernel;create_parent_user_seconds=$creationUser;wall_to_exit_seconds=$clock.Elapsed.TotalSeconds;child_cpu_seconds=$child.TotalProcessorTime.TotalSeconds;exit_code=$child.ExitCode;timed_out=$timeout}
 $stdout.Result | Out-Null;$stderr.Result | Out-Null;$child.Dispose()
}
@{schema_version=1;scope='Synthetic FFmpeg -version image-launch locality diagnostic. No desktop capture, no encoding and no product performance claim.';binary_sha256=$sourceHash.ToLowerInvariant();binary_bytes=(Get-Item -LiteralPath $Source).Length;runs=$runs} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputRoot 'recording_image_io.json') -Encoding utf8
Write-Output 'Completed mapped/local identical-image diagnostic.'

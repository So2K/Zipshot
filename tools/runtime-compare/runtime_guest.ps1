# Disposable guest job runner. Never run this on the host.
$ErrorActionPreference='Stop'
$inputRoot='C:/ZipshotLab/input';$outputRoot='C:/ZipshotLab/output';$workRoot='C:/ZipshotLab/work'
if($env:USERNAME -ne 'WDAGUtilityAccount' -or [Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0 -or -not(Test-Path "$inputRoot/.isolation-approved.json")){throw 'Isolated guest identity is required.'}
$approval=Get-Content "$inputRoot/.isolation-approved.json" -Raw | ConvertFrom-Json
if($approval.task -ne 'zipshot-runtime-compare-20261118'){throw 'Unknown task marker.'}
$env:ZIPSHOT_RUNTIME_ISOLATED='1'
New-Item -ItemType Directory -Path $outputRoot,$workRoot -Force | Out-Null
Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public static class RuntimeDesktop {
 [DllImport("user32.dll")]public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
 [DllImport("user32.dll")]public static extern int GetSystemMetrics(int i);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern bool SystemParametersInfo(uint a,uint b,string c,uint d);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr FindWindow(string cls,string title);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr FindWindowEx(IntPtr p,IntPtr after,string cls,string title);
 [DllImport("user32.dll")]public static extern bool ShowWindow(IntPtr h,int command);
}
'@
[RuntimeDesktop]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
# Guest preferences only, identical for both applications. No host setting changes.
New-Item -Path 'HKCU:/Software/Microsoft/Windows/CurrentVersion/Themes/Personalize' -Force | Out-Null
Set-ItemProperty -Path 'HKCU:/Software/Microsoft/Windows/CurrentVersion/Themes/Personalize' -Name AppsUseLightTheme -Value 0
Set-ItemProperty -Path 'HKCU:/Control Panel/Keyboard' -Name PrintScreenKeyForSnippingEnabled -Value 0
[RuntimeDesktop]::SystemParametersInfo(20,0,"$inputRoot/wallpaper.png",3) | Out-Null
$progman=[RuntimeDesktop]::FindWindow('Progman',$null)
$view=[RuntimeDesktop]::FindWindowEx($progman,[IntPtr]::Zero,'SHELLDLL_DefView',$null)
$icons=[RuntimeDesktop]::FindWindowEx($view,[IntPtr]::Zero,'SysListView32',$null)
if($icons -ne [IntPtr]::Zero){[RuntimeDesktop]::ShowWindow($icons,0) | Out-Null}

foreach($app in @('sharex18','sharex22')){
 $personal=Join-Path $workRoot $app;$save=Join-Path $workRoot "shots/$app"
 New-Item -ItemType Directory -Path $personal,$save -Force | Out-Null
 $config=[ordered]@{ShowStartScreen=$false;ShowIconPackPrompt=$false;FirstTimeMinimizeToTray=$false;AutoCheckUpdate=$false;ShowTray=$true;ShowClipboardContentViewer=$false;DisableUpload=$true;UseCustomScreenshotsPath=$true;CustomScreenshotsPath=$save;SaveImageSubFolderPattern='';Language='English';DefaultTaskSettings=@{AfterCaptureJob='CopyImageToClipboard, SaveImageToFile';AfterUploadJob='None';ImageSettings=@{ImageFormat='PNG';ImageAutoUseJPEG=$false};CaptureSettings=@{ShowCursor=$false};GeneralSettings=@{PlaySoundAfterCapture=$false;PlaySoundAfterUpload=$false;PlaySoundAfterAction=$false;ShowToastNotificationAfterTaskCompleted=$true;ToastWindowDuration=0.6;ToastWindowFadeDuration=0}}}
 $config | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $personal 'ApplicationConfig.json') -Encoding UTF8
}
New-Item -ItemType Directory -Path "$workRoot/shots/zipshot" -Force | Out-Null
$ready=[ordered]@{status='ready';guest=$env:USERNAME;computer=$env:COMPUTERNAME;session=[Diagnostics.Process]::GetCurrentProcess().SessionId;pid=$PID;utc=[DateTime]::UtcNow.ToString('o');display=@{width=[RuntimeDesktop]::GetSystemMetrics(0);height=[RuntimeDesktop]::GetSystemMetrics(1)};os=Get-CimInstance Win32_OperatingSystem | Select-Object Caption,BuildNumber;gpu=@(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion);logical_processors=[Environment]::ProcessorCount}
$ready | ConvertTo-Json -Depth 6 | Set-Content "$outputRoot/guest-ready.json" -Encoding UTF8
$seen=@{};$lastWork=[DateTime]::UtcNow;$deadline=$lastWork.AddHours(2)
try {
 while([DateTime]::UtcNow -lt $deadline){
  $jobs=@(Get-ChildItem "$inputRoot/jobs" -Filter '*.json' -File | Sort-Object Name)
  foreach($file in $jobs){
   if($seen.ContainsKey($file.Name)){continue};$seen[$file.Name]=$true;$lastWork=[DateTime]::UtcNow
   $job=Get-Content $file.FullName -Raw | ConvertFrom-Json
   if($job.id -notmatch '^[a-zA-Z0-9_-]+$'){throw 'Invalid job id'}
   if($job.kind -eq 'shutdown'){'stopped' | Set-Content "$outputRoot/guest-stopped.txt";return}
   $allowed=@{capture='runtime_capture_job.ps1';recording='recording/recording_guest.ps1';editor='runtime_editor_job.ps1';inspect='runtime_inspect_job.ps1'}
   if(-not $allowed.ContainsKey($job.kind)){throw 'Unsupported job kind'}
   $report=[ordered]@{id=$job.id;kind=$job.kind;start_utc=[DateTime]::UtcNow.ToString('o');status='running'}
   $report | ConvertTo-Json | Set-Content "$outputRoot/$($job.id)-status.json" -Encoding UTF8
   try {
    $script=Join-Path $inputRoot $allowed[$job.kind]
    $invoke='& '+"'"+$script+"'"
    foreach($argument in $job.arguments){
     # Parameter names are syntax, values are literals. Quoting '-Seconds'
     # would turn it into a positional string instead of a named parameter.
     if([string]$argument -match '^-[A-Za-z][A-Za-z0-9]*$'){$invoke+=' '+[string]$argument}
     else{$invoke+=' '+"'"+([string]$argument).Replace("'","''")+"'"}
    }
    $encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($invoke))
    $psi=New-Object Diagnostics.ProcessStartInfo
    $psi.FileName="$env:WINDIR/System32/WindowsPowerShell/v1.0/powershell.exe"
    $psi.Arguments='-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand '+$encoded
    $psi.UseShellExecute=$false;$psi.CreateNoWindow=$true;$psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
    $process=[Diagnostics.Process]::Start($psi);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $limit=if($job.timeout_seconds){[int]$job.timeout_seconds}else{900}
    if(-not $process.WaitForExit($limit*1000)){$report.status='timeout';$process.Kill()}else{$report.exit_code=$process.ExitCode;$report.status=if($process.ExitCode -eq 0){'pass'}else{'fail'}}
    $stdout.Result | Set-Content "$outputRoot/$($job.id)-stdout.txt" -Encoding UTF8
    $stderr.Result | Set-Content "$outputRoot/$($job.id)-stderr.txt" -Encoding UTF8
   }catch{$report.status='fail';$report.error=$_.Exception.Message}
   $report.end_utc=[DateTime]::UtcNow.ToString('o');$report | ConvertTo-Json -Depth 6 | Set-Content "$outputRoot/$($job.id)-status.json" -Encoding UTF8
  }
  if(([DateTime]::UtcNow-$lastWork).TotalMinutes -gt 30){break}
  Start-Sleep -Milliseconds 250
 }
} finally {
 'stopped' | Set-Content "$outputRoot/guest-stopped.txt"
 & "$env:WINDIR/System32/shutdown.exe" /s /t 0
}

# Called only by the owned Windows Sandbox configuration. Never run on the host.
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount') { throw 'Refusing to run outside Windows Sandbox guest.' }
$inputRoot='C:/ZipshotLab/input'
$outputRoot='C:/ZipshotLab/output'
$result=[ordered]@{schema_version=1;status='running';guest_username=$env:USERNAME;guest_computer=$env:COMPUTERNAME;guest_script_pid=$PID;guest_session_id=[Diagnostics.Process]::GetCurrentProcess().SessionId;utc_start=[DateTime]::UtcNow.ToString('o')}
try {
    $os=Get-CimInstance Win32_OperatingSystem
    $result.os=@{caption=$os.Caption;version=$os.Version;build=$os.BuildNumber}
    $result.gpu=@(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion)
    $probe=Start-Process -FilePath "$inputRoot/capture_probe.exe" -PassThru
    $deadline=[DateTime]::UtcNow.AddSeconds(45)
    while(-not(Test-Path -LiteralPath "$outputRoot/capture_native_ready.flag")) {
        if($probe.HasExited) { throw "Native guest probe exited before readiness: $($probe.ExitCode)" }
        if([DateTime]::UtcNow -gt $deadline) { throw 'Native guest fixture readiness timeout.' }
        Start-Sleep -Milliseconds 100
    }
    $psi=New-Object Diagnostics.ProcessStartInfo
    $psi.FileName="$inputRoot/ffmpeg.exe"
    $psi.Arguments="-hide_banner -y -probesize 32 -analyzeduration 0 -f gdigrab -framerate 60 -draw_mouse 0 -i desktop -t 4 -an -c:v libx264 -preset ultrafast -crf 0 -threads 2 -pix_fmt yuv420p -fps_mode passthrough $outputRoot/capture_probe.mkv"
    $psi.UseShellExecute=$false
    $psi.CreateNoWindow=$true
    $psi.RedirectStandardError=$true
    $record=[Diagnostics.Process]::Start($psi)
    $recordError=$record.StandardError.ReadToEndAsync()
    Start-Sleep -Milliseconds 700
    if($record.HasExited) { throw "Guest FFmpeg exited before input: $($record.ExitCode)" }
    'go' | Set-Content -LiteralPath "$outputRoot/capture_go.flag"
    if(-not $probe.WaitForExit(20000)) { throw 'Native guest probe completion timeout.' }
    if(-not $record.WaitForExit(20000)) { throw 'Guest FFmpeg completion timeout.' }
    $recordError.Result | Set-Content -LiteralPath "$outputRoot/capture_ffmpeg.log"
    if($probe.ExitCode -ne 0 -or $record.ExitCode -ne 0) { throw "Guest exit failure: native=$($probe.ExitCode),ffmpeg=$($record.ExitCode)" }
    & "$inputRoot/ffmpeg.exe" -v error -y -i "$outputRoot/capture_fixture.bmp" -frames:v 1 "$outputRoot/capture_fixture.png" 2> "$outputRoot/capture_png.log"
    $ffprobe=& "$inputRoot/ffprobe.exe" -v error -count_frames -show_streams -show_format -of json "$outputRoot/capture_probe.mkv"
    if($LASTEXITCODE -ne 0) { throw 'Guest ffprobe failed.' }
    $result.ffprobe=$ffprobe | ConvertFrom-Json
    $result.native=Get-Content -LiteralPath "$outputRoot/capture_native_result.json" -Raw | ConvertFrom-Json
    $result.status=if($result.native.status -eq 'pass') {'pass'} else {'fail'}
} catch {
    $result.status='fail'
    $result.error=$_.Exception.Message
} finally {
    $result.utc_end=[DateTime]::UtcNow.ToString('o')
    $result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath "$outputRoot/capture_guest_result.json" -Encoding UTF8
    # This process has already verified the disposable guest identity; shut down ONLY that guest.
    & "$env:WINDIR/System32/shutdown.exe" /s /t 0
}

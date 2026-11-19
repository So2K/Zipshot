# Guest-only no-input smoke probe. It launches one owned editor and requests evidence.
param([ValidateSet('native','sharex22')][string]$Engine='native',[string]$Fixture='C:/ZipshotLab/input/fixtures/editor-fixture-1920x1080.png',[string]$OutputRoot='C:/ZipshotLab/output/editor-probes',[string]$NativeExe='C:/ZipshotLab/input/editor_ui_native.exe',[string]$ShareXExe='C:/ZipshotLab/input/editor22/ShareX.exe')
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount' -or [Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0 -or $env:ZIPSHOT_RUNTIME_ISOLATED -ne '1' -or -not(Test-Path -LiteralPath 'C:/ZipshotLab/input/.isolation-approved.json')){throw 'Refusing outside the approved Windows Sandbox guest'}
$run=Join-Path $OutputRoot ($Engine+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $run -Force|Out-Null
$prefix=Join-Path $run 'editor'
$result=[ordered]@{schema_version=1;engine=$Engine;utc_start=[DateTime]::UtcNow.ToString('o');status='running';no_input_injected=$true;guest_user=$env:USERNAME;guest_session=[Diagnostics.Process]::GetCurrentProcess().SessionId;output_prefix=$prefix}
$process=$null;$stdout=$null;$stderr=$null
function Read-PngSize([string]$Path){
    $bytes=[IO.File]::ReadAllBytes($Path)
    if($bytes.Length -lt 24 -or [BitConverter]::ToString($bytes[0..7]) -ne '89-50-4E-47-0D-0A-1A-0A'){throw "Not a PNG $Path"}
    $wBytes=[byte[]]$bytes[16..19];[Array]::Reverse($wBytes)
    $hBytes=[byte[]]$bytes[20..23];[Array]::Reverse($hBytes)
    return @([BitConverter]::ToUInt32($wBytes,0),[BitConverter]::ToUInt32($hBytes,0))
}
try{
    $exe=if($Engine -eq 'native'){$NativeExe}else{$ShareXExe}
    if(-not(Test-Path -LiteralPath $exe)-or -not(Test-Path -LiteralPath $Fixture)){throw 'Helper or fixture is missing'}
    $info=New-Object Diagnostics.ProcessStartInfo
    $info.FileName=$exe
    $editorArguments=@($Fixture,$prefix,'1280','900','5000')
    $info.Arguments=(@($editorArguments|ForEach-Object {'"'+$_+'"'})-join ' ')
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $process=[Diagnostics.Process]::Start($info)
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $result.pid=$process.Id
    $deadline=[DateTime]::UtcNow.AddSeconds(40)
    while(-not(Test-Path -LiteralPath ($prefix+'-ready.json'))){
        if($process.HasExited){throw "Editor exited before readiness with code $($process.ExitCode)"}
        if([DateTime]::UtcNow -gt $deadline){throw 'Editor readiness deadline exceeded'}
        Start-Sleep -Milliseconds 100
    }
    $ready=Get-Content -LiteralPath ($prefix+'-ready.json') -Raw|ConvertFrom-Json
    if([int]$ready.pid -ne $process.Id){throw 'Readiness process identity mismatch'}
    $result.ready=$ready
    if($Engine -eq 'native'){
        if(-not('EditorProbeMessage' -as [type])){Add-Type @'
using System; using System.Runtime.InteropServices;
public static class EditorProbeMessage {
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
 [DllImport("user32.dll",SetLastError=true)] public static extern IntPtr SendMessageTimeoutW(IntPtr window,uint message,IntPtr w,IntPtr l,uint flags,uint timeout,out UIntPtr result);
}
'@
        }
        $hwnd=[IntPtr]::new([Convert]::ToInt64([string]$ready.hwnd,16));[uint32]$windowPid=0
        [EditorProbeMessage]::GetWindowThreadProcessId($hwnd,[ref]$windowPid)|Out-Null
        if($windowPid -ne $process.Id){throw 'Snapshot window belongs to another process'}
        $reply=[UIntPtr]::Zero
        $sent=[EditorProbeMessage]::SendMessageTimeoutW($hwnd,0x8000+302,[IntPtr]::Zero,[IntPtr]::Zero,2,10000,[ref]$reply)
        if($sent -eq [IntPtr]::Zero -or $reply -eq [UIntPtr]::Zero){throw 'Native actual-frame/document snapshot failed'}
    }else{
        [IO.File]::WriteAllText($prefix+'-command.txt','snapshot')
    }
    $snapshotDeadline=[DateTime]::UtcNow.AddSeconds(12)
    while(-not(Test-Path -LiteralPath ($prefix+'-snapshot.json'))){
        if([DateTime]::UtcNow -gt $snapshotDeadline){throw 'Annotated export snapshot deadline exceeded'}
        Start-Sleep -Milliseconds 100
    }
    if(-not $process.WaitForExit(20000)){throw 'Owned editor did not finish in its configured lifetime'}
    $result.exit_code=$process.ExitCode
    if($process.ExitCode -ne 0){throw "Editor helper failed with code $($process.ExitCode)"}
    foreach($suffix in @('-snapshot.png','-snapshot.json','-final.json','-events.json')){if(-not(Test-Path -LiteralPath ($prefix+$suffix))){throw "Missing evidence $suffix"}}
    $snapshot=Get-Content -LiteralPath ($prefix+'-snapshot.json') -Raw|ConvertFrom-Json
    $result.snapshot=$snapshot
    $result.snapshot_dimensions=Read-PngSize ($prefix+'-snapshot.png')
    $result.snapshot_sha256=(Get-FileHash -LiteralPath ($prefix+'-snapshot.png')).Hash
    if($result.snapshot_dimensions[0] -lt 64 -or $result.snapshot_dimensions[1] -lt 64){throw 'Snapshot dimensions are invalid'}
    if($Engine -eq 'native'){
        if(-not $snapshot.window_capture_ok -or $snapshot.window_nonuniform_pixels -lt 100){throw 'Native displayed-window capture is blank or unavailable'}
        $result.window_dimensions=Read-PngSize ($prefix+'-window.png')
        $result.window_sha256=(Get-FileHash -LiteralPath ($prefix+'-window.png')).Hash
    }
    $result.final=Get-Content -LiteralPath ($prefix+'-final.json') -Raw|ConvertFrom-Json
    $result.status='pass'
    $result.limit='No physical input was injected. Actual visual inspection and timestamped recording remain separate checks. ShareX snapshot is its annotated export, not a desktop screenshot.'
}catch{
    $result.status='fail';$result.error=$_.Exception.Message
}finally{
    if($null -ne $process -and -not $process.HasExited){
        # Only the exact disposable-guest process launched above is eligible for cleanup.
        if($Engine -eq 'sharex22'){[IO.File]::WriteAllText($prefix+'-command.txt','stop')}
        if(-not $process.WaitForExit(3000)){$process.Kill();$process.WaitForExit();$result.owned_process_cleanup='Terminated the exact helper process started by this failed probe'}
    }
    if($null -ne $process -and $process.HasExited){$stdout.Result|Set-Content -LiteralPath (Join-Path $run 'stdout.txt');$stderr.Result|Set-Content -LiteralPath (Join-Path $run 'stderr.txt')}
    $result.utc_end=[DateTime]::UtcNow.ToString('o')
    $result|ConvertTo-Json -Depth 15|Set-Content -LiteralPath (Join-Path $run 'editor-probe-result.json') -Encoding UTF8
    Write-Output (Join-Path $run 'editor-probe-result.json')
}

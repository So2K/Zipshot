param([string]$StageRoot='D:/ZipshotRuntimeCompare-20261118')
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$StageRoot=[IO.Path]::GetFullPath($StageRoot)
if($StageRoot -eq [IO.Path]::GetPathRoot($StageRoot)) { throw 'Refusing a drive-root staging directory.' }
if(Get-Process -Name WindowsSandbox,WindowsSandboxClient -ErrorAction SilentlyContinue) { throw 'A Sandbox is already running; do not reuse or disturb it.' }
$inputRoot=Join-Path $StageRoot 'sandbox-input'
$outputRoot=Join-Path $StageRoot 'sandbox-output'
New-Item -ItemType Directory -Path $inputRoot,$outputRoot -Force | Out-Null
if(Get-ChildItem -LiteralPath $outputRoot -File -ErrorAction SilentlyContinue) { throw 'Probe outputs already exist; preserve them and choose a fresh StageRoot.' }
Copy-Item -LiteralPath (Join-Path $repo 'build/runtime-compare/capture_probe.exe') -Destination $inputRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'capture_probe_guest.ps1') -Destination $inputRoot
foreach($name in @('ffmpeg','ffprobe')) {
    Copy-Item -LiteralPath (Get-Command $name).Source -Destination (Join-Path $inputRoot "$name.exe")
}
$encode={param($value) [Security.SecurityElement]::Escape($value)}
$inputXml=& $encode $inputRoot
$outputXml=& $encode $outputRoot
$config=@"
<Configuration>
  <vGPU>Enable</vGPU>
  <MemoryInMB>4096</MemoryInMB>
  <Networking>Disable</Networking>
  <ClipboardRedirection>Disable</ClipboardRedirection>
  <AudioInput>Disable</AudioInput>
  <VideoInput>Disable</VideoInput>
  <PrinterRedirection>Disable</PrinterRedirection>
  <MappedFolders>
    <MappedFolder><HostFolder>$inputXml</HostFolder><SandboxFolder>C:\ZipshotLab\input</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$outputXml</HostFolder><SandboxFolder>C:\ZipshotLab\output</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
  </MappedFolders>
  <LogonCommand><Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\ZipshotLab\input\capture_probe_guest.ps1</Command></LogonCommand>
</Configuration>
"@
$wsb=Join-Path $StageRoot 'capture_probe.wsb'
$config | Set-Content -LiteralPath $wsb -Encoding UTF8
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CaptureHostFocus {
 public delegate bool EnumProc(IntPtr hwnd,IntPtr parameter);
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int Left,Top,Right,Bottom;}
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback,IntPtr parameter);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd,out Rect rect);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd,out Rect rect);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd,IntPtr after,int x,int y,int width,int height,uint flags);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd,uint message,IntPtr wparam,IntPtr lparam);
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
}
'@
$prior=[CaptureHostFocus]::GetForegroundWindow()
[CaptureHostFocus]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null
[uint32]$priorPid=0; [CaptureHostFocus]::GetWindowThreadProcessId($prior,[ref]$priorPid) | Out-Null
$process=Start-Process -FilePath "$env:WINDIR/System32/WindowsSandbox.exe" -ArgumentList ('"'+$wsb+'"') -WindowStyle Hidden -PassThru
$deadline=[DateTime]::UtcNow.AddSeconds(150)
$focusRestored=$false
$resized=@{}
while([DateTime]::UtcNow -lt $deadline -and -not(Test-Path -LiteralPath "$outputRoot/capture_guest_result.json")) {
    $clientPids=@(Get-Process -Name WindowsSandboxClient -ErrorAction SilentlyContinue | ForEach-Object Id)
    [CaptureHostFocus]::EnumWindows({param($hwnd,$parameter)
        [uint32]$windowPid=0; [CaptureHostFocus]::GetWindowThreadProcessId($hwnd,[ref]$windowPid) | Out-Null
        if($clientPids -contains [int]$windowPid) {
            $wr=New-Object CaptureHostFocus+Rect; $cr=New-Object CaptureHostFocus+Rect
            [CaptureHostFocus]::GetWindowRect($hwnd,[ref]$wr) | Out-Null
            [CaptureHostFocus]::GetClientRect($hwnd,[ref]$cr) | Out-Null
            if($cr.Right -gt 320 -and $cr.Bottom -gt 200 -and ($cr.Right -ne 1280 -or $cr.Bottom -ne 720)) {
                $windowWidth=1280+($wr.Right-$wr.Left)-$cr.Right
                $windowHeight=720+($wr.Bottom-$wr.Top)-$cr.Bottom
                # Only the newly created Sandbox client's own window; preserve location, z-order and focus.
                $ok=[CaptureHostFocus]::SetWindowPos($hwnd,[IntPtr]::Zero,0,0,$windowWidth,$windowHeight,0x16)
                [CaptureHostFocus]::SendMessage($hwnd,0x232,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
                if($ok) { $resized[$hwnd.ToInt64()]=@{pid=$windowPid;requested_client_width=1280;requested_client_height=720} }
            }
        }
        return $true
    },[IntPtr]::Zero) | Out-Null
    $current=[CaptureHostFocus]::GetForegroundWindow()
    if($current -ne $prior) {
        [uint32]$focusPid=0; [CaptureHostFocus]::GetWindowThreadProcessId($current,[ref]$focusPid) | Out-Null
        $focusProcess=Get-Process -Id $focusPid -ErrorAction SilentlyContinue
        if($focusProcess.ProcessName -like 'WindowsSandbox*') { $focusRestored=[CaptureHostFocus]::SetForegroundWindow($prior) }
    }
    Start-Sleep -Milliseconds 250
}
[ordered]@{host_launcher_pid=$process.Id;prior_foreground_pid=$priorPid;foreground_restored_if_sandbox_stole_it=$focusRestored;owned_client_window_resizes=@($resized.Values);stage_root=$StageRoot;guest_result_present=(Test-Path -LiteralPath "$outputRoot/capture_guest_result.json");utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $StageRoot 'capture_host_result.json') -Encoding UTF8
if(Test-Path -LiteralPath "$outputRoot/capture_guest_result.json") { Get-Content -LiteralPath "$outputRoot/capture_guest_result.json" -Raw }
else { throw 'Sandbox guest did not produce a result within 150 seconds. Do not assume launch or capture success.' }

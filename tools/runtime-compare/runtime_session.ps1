param([ValidateSet('Start','Submit','Status','Shutdown')][string]$Action='Status',[string]$StageRoot='D:/ZipshotRuntimeCompare-20261118/session01',[string]$Id='', [string]$Kind='', [string[]]$JobArguments=@(),[int]$TimeoutSeconds=900)
$ErrorActionPreference='Stop'
$StageRoot=[IO.Path]::GetFullPath($StageRoot)
if(-not(Test-Path "$StageRoot/.zipshot-runtime-session.json")){throw 'Prepare the task-owned stage first.'}
$inputRoot=Join-Path $StageRoot 'input';$outputRoot=Join-Path $StageRoot 'output'
if($Action -eq 'Status'){
 if(Test-Path "$outputRoot/guest-ready.json"){Get-Content "$outputRoot/guest-ready.json" -Raw}
 Get-ChildItem "$outputRoot" -Filter '*-status.json' | ForEach-Object {Get-Content $_.FullName -Raw}
 exit
}
if($Action -eq 'Submit' -or $Action -eq 'Shutdown'){
 if($Action -eq 'Shutdown'){$Kind='shutdown';$Id='zz-shutdown-'+(Get-Date -Format HHmmss)}
 if($Id -notmatch '^[a-zA-Z0-9_-]+$' -or $Kind -notin @('capture','recording','editor','inspect','shutdown')){throw 'Invalid job'}
 $job=[ordered]@{id=$Id;kind=$Kind;arguments=$JobArguments;timeout_seconds=$TimeoutSeconds}
 $target=Join-Path $inputRoot "jobs/$Id.json"
 if(Test-Path $target){throw 'Job id already exists'}
 $job | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath ($target+'.tmp') -Encoding UTF8
 Move-Item -LiteralPath ($target+'.tmp') -Destination $target
 Write-Output ('Submitted '+$Id);exit
}
if(Get-Process -Name WindowsSandbox,WindowsSandboxClient -ErrorAction SilentlyContinue){throw 'A Sandbox client is already running. Preserve it.'}
$ix=[Security.SecurityElement]::Escape($inputRoot);$ox=[Security.SecurityElement]::Escape($outputRoot)
$wsb=Join-Path $StageRoot 'runtime.wsb'
@"
<Configuration><vGPU>Enable</vGPU><MemoryInMB>6144</MemoryInMB><Networking>Disable</Networking><ClipboardRedirection>Disable</ClipboardRedirection><AudioInput>Disable</AudioInput><VideoInput>Disable</VideoInput><PrinterRedirection>Disable</PrinterRedirection><MappedFolders><MappedFolder><HostFolder>$ix</HostFolder><SandboxFolder>C:\ZipshotLab\input</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder><MappedFolder><HostFolder>$ox</HostFolder><SandboxFolder>C:\ZipshotLab\output</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder></MappedFolders><LogonCommand><Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\ZipshotLab\input\runtime_guest.ps1</Command></LogonCommand></Configuration>
"@ | Set-Content -LiteralPath $wsb -Encoding UTF8
Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;public static class RuntimeHostFocus{
 [DllImport("user32.dll")]public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
}
'@
$prior=[RuntimeHostFocus]::GetForegroundWindow();[uint32]$priorPid=0;[RuntimeHostFocus]::GetWindowThreadProcessId($prior,[ref]$priorPid)|Out-Null
$launcher=Start-Process "$env:WINDIR/System32/WindowsSandbox.exe" -ArgumentList ('"'+$wsb+'"') -WindowStyle Hidden -PassThru
$deadline=[DateTime]::UtcNow.AddSeconds(180);$restored=$false
while(-not(Test-Path "$outputRoot/guest-ready.json") -and [DateTime]::UtcNow -lt $deadline){
 $current=[RuntimeHostFocus]::GetForegroundWindow()
 if($current -ne $prior){[uint32]$focusPid=0;[RuntimeHostFocus]::GetWindowThreadProcessId($current,[ref]$focusPid)|Out-Null;$p=Get-Process -Id $focusPid -ErrorAction SilentlyContinue;if($p.ProcessName -like 'WindowsSandbox*'){$restored=[RuntimeHostFocus]::SetForegroundWindow($prior)}}
 Start-Sleep -Milliseconds 250
}
$record=@{launcher_pid=$launcher.Id;prior_foreground_pid=$priorPid;restored_if_needed=$restored;guest_ready=(Test-Path "$outputRoot/guest-ready.json");utc=[DateTime]::UtcNow.ToString('o')}
$record | ConvertTo-Json | Set-Content "$StageRoot/host-session.json" -Encoding UTF8
if(-not $record.guest_ready){throw 'No verified guest readiness after three minutes.'}
Get-Content "$outputRoot/guest-ready.json" -Raw

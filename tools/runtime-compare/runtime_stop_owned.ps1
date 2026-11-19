param([string]$StageRoot='D:/ZipshotRuntimeCompare-20261118/session01')
$ErrorActionPreference='Stop'
if(-not(Test-Path "$StageRoot/.zipshot-runtime-session.json") -or -not(Test-Path "$StageRoot/host-session.json")){throw 'Unknown session ownership'}
if(-not(Test-Path "$StageRoot/output/guest-stopped.txt")){throw 'Guest has not acknowledged shutdown. Submit shutdown first.'}
$session=Get-Content "$StageRoot/host-session.json" -Raw | ConvertFrom-Json
$start=[DateTime]::Parse($session.utc).ToUniversalTime().AddMinutes(-5)
$clients=@(Get-Process -Name WindowsSandboxClient -ErrorAction SilentlyContinue)
foreach($client in $clients){
 if($client.StartTime.ToUniversalTime() -lt $start){throw 'Sandbox client predates this owned session; refusing to stop it.'}
}
# The guest has already shut down and all outputs are in the mapped task folder.
# Stop only its remaining client/feedback process, never any unrelated app.
foreach($client in $clients){Stop-Process -Id $client.Id;Write-Output ('Closed owned Sandbox client '+$client.Id)}

param([ValidateSet('legacy','current')][string]$Engine='legacy',[int]$Seconds=3,[string]$Cases='zipshot-video30,sharex-video30,zipshot-gif15,sharex-gif15',[string]$OutputRoot='C:/ZipshotLab/output/recording-pilot',[ValidateSet('video','gif')][string]$Mode='video',[ValidateSet('native-hardware','native-software-allowed')][string]$Profile='native-hardware',[int]$FPS=30,[switch]$SkipSetupBar,[ValidateSet('factory','veryfast','ultrafast')][string]$SoftwarePreset='veryfast')
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount' -or -not(Test-Path 'C:/ZipshotLab/input/.isolation-approved.json')){throw 'Guest only'}
if($Engine -eq 'legacy'){
 & 'C:/ZipshotLab/input/recording/recording_guest_legacy.ps1' -InputRoot 'C:/ZipshotLab/input/recording' -OutputRoot $OutputRoot -Seconds $Seconds -Cases $Cases -SoftwarePreset $SoftwarePreset
}else{
 & 'C:/ZipshotLab/input/recording/recording_sharex22_guest.ps1' -InputRoot 'C:/ZipshotLab/input' -OutputRoot $OutputRoot -Mode $Mode -Profile $Profile -FPS $FPS -Seconds $Seconds -SkipSetupBar:$SkipSetupBar
}

# Generic editor entry point for the serial disposable-guest job runner.
param([ValidateSet('native','sharex22')][string]$Engine='native',[ValidateSet('probe','interaction','stockprobe')][string]$Mode='probe',[string]$Fixture='C:/ZipshotLab/input/fixtures/editor-fixture-1920x1080.png',[string]$OutputRoot='C:/ZipshotLab/output/editor-probes',[string]$Case='arrow',[string]$NativeExe='C:/ZipshotLab/input/editor_ui_native.exe',[string]$ShareXExe='C:/ZipshotLab/input/editor22/ShareX.exe',[string]$ProbeScript='C:/ZipshotLab/input/editor_ui_probe.ps1',[string]$StockProbeScript='C:/ZipshotLab/input/editor_stock22_probe.ps1',[string]$InteractionScript='C:/ZipshotLab/input/editor_ui_interaction.ps1')
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount' -or [Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0 -or $env:ZIPSHOT_RUNTIME_ISOLATED -ne '1' -or -not(Test-Path -LiteralPath 'C:/ZipshotLab/input/.isolation-approved.json')){throw 'Refusing outside approved isolated guest'}
if($Mode -eq 'stockprobe'){
    $paths=@(& $StockProbeScript -Fixture $Fixture -OutputRoot $OutputRoot)
    $path=[string]$paths[-1];Write-Output $path
    $result=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
    if($result.status -eq 'fail'){throw ('Stock editor probe failed. '+$result.error)}
}elseif($Mode -eq 'probe'){
    $paths=@(& $ProbeScript -Engine $Engine -Fixture $Fixture -OutputRoot $OutputRoot -NativeExe $NativeExe -ShareXExe $ShareXExe)
    $path=[string]$paths[-1]
    if(-not(Test-Path -LiteralPath $path)){throw 'Editor probe returned no result file'}
    $result=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
    Write-Output $path
    if($result.status -ne 'pass'){throw ('Editor probe failed. '+$result.error)}
}else{
    if(-not(Test-Path -LiteralPath $InteractionScript)){throw 'Interaction driver has not been staged'}
    $paths=@(& $InteractionScript -Engine $Engine -Fixture $Fixture -OutputRoot $OutputRoot -Case $Case -NativeExe $NativeExe -ShareXExe $ShareXExe)
    $path=[string]$paths[-1];Write-Output $path
    if(-not(Test-Path -LiteralPath $path)){throw 'No interaction report'}
    $result=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
    if($result.status -eq 'fail'){throw ('Editor interaction failed. '+$result.error)}
}

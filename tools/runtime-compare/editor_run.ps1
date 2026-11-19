param([int]$Blocks=3,[int]$Samples=10,[int]$Warmups=3,[string]$ShareXDir='D:/Programs/ShareX',[string]$RuntimeStage='D:/Tools/ZipshotEditorBenchmark20261118')
$ErrorActionPreference='Stop'
if($Blocks -lt 1 -or $Samples -lt 1 -or $Warmups -lt 0){throw 'Invalid sample counts'}
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$native=Join-Path $root 'build/runtime-compare/editor-native/editor_native.exe'
$managed=Join-Path $RuntimeStage 'ShareX.exe'
$fixtures=Join-Path $root 'build/runtime-compare/editor-fixtures'
$out=Join-Path $root ('build/runtime-compare/editor-results/run-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $out -Force|Out-Null
$runs=[Collections.Generic.List[object]]::new()
function Start-EditorExport([string]$Engine,[string]$Fixture,[string]$Case,[int]$Block){
    $size=[IO.Path]::GetFileNameWithoutExtension($Fixture)-replace 'editor-fixture-',''
    $stem="$Engine-$size-$Case-block$Block"
    $png=Join-Path $out ($stem+'.png');$json=Join-Path $out ($stem+'.json');$err=Join-Path $out ($stem+'.err')
    $exe=if($Engine -eq 'zipshot'){$native}else{$managed}
    $exportArguments=@($Fixture,$Case,$Warmups,$Samples,$png)
    if($Engine -eq 'sharex'){$exportArguments=@($ShareXDir)+$exportArguments}
    $quoted=@($exportArguments|ForEach-Object {'"'+$_+'"'})
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $process=Start-Process -FilePath $exe -ArgumentList $quoted -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $json -RedirectStandardError $err
    $clock.Stop()
    if($process.ExitCode -ne 0){throw "$stem failed with exit $($process.ExitCode), inspect $err"}
    $result=Get-Content -LiteralPath $json -Raw|ConvertFrom-Json
    if($result.ms.Count -ne $Samples){throw "$stem wrong count"}
    $runs.Add([pscustomobject]@{engine=$Engine;block=$Block;case=$Case;width=$result.width;height=$result.height;ms=@($result.ms);changed_pixels=$result.changed_pixels;pixel_signature=$result.pixel_signature;assembly_version=$result.assembly_version;runtime_version=$result.runtime_version;output=[IO.Path]::GetFileName($png);output_sha256=(Get-FileHash -LiteralPath $png).Hash;process_wall_ms=$clock.Elapsed.TotalMilliseconds})
    Write-Output "$stem median $((@($result.ms|Sort-Object))[ [int][Math]::Floor($Samples/2)]) ms"
}
foreach($resolution in @('1920x1080','3840x2160')){
    $fixture=Join-Path $fixtures "editor-fixture-$resolution.png"
    foreach($case in @('plain','arrow','arrows32','freehand','blur_cold','blur_warm')){
        for($block=0;$block -lt $Blocks;$block++){
            $order=if($block%2 -eq 0){@('zipshot','sharex')}else{@('sharex','zipshot')}
            foreach($engine in $order){Start-EditorExport $engine $fixture $case $block}
        }
    }
}
$summary=@($runs|Group-Object engine,width,height,case|ForEach-Object {
    $group=$_.Group;$values=@($group|ForEach-Object {$_.ms}|Sort-Object)
    $mid=$values.Count/2
    $median=if($values.Count%2){$values[[int][Math]::Floor($mid)]}else{($values[[int]$mid-1]+$values[[int]$mid])/2}
    [ordered]@{engine=$group[0].engine;case=$group[0].case;width=$group[0].width;height=$group[0].height;n=$values.Count;median_ms=$median;p95_ms=$values[[int][Math]::Ceiling($values.Count*.95)-1];max_ms=$values[-1];min_ms=$values[0]}
})
$provenance=[ordered]@{zipshot_git=(git -C $root rev-parse HEAD);zipshot_editor_sha256=(Get-FileHash -LiteralPath (Join-Path $root 'src/editor.cpp')).Hash;native_harness_sha256=(Get-FileHash -LiteralPath $native).Hash;managed_harness_sha256=(Get-FileHash -LiteralPath (Join-Path $RuntimeStage 'ShareX.dll')).Hash;sharex_screen_capture_sha256=(Get-FileHash -LiteralPath (Join-Path $ShareXDir 'ShareX.ScreenCaptureLib.dll')).Hash;fixture_manifest=(Get-Content -LiteralPath (Join-Path $fixtures 'editor-fixture-manifest.json') -Raw|ConvertFrom-Json)}
$report=[ordered]@{schema_version=1;utc=[DateTime]::UtcNow.ToString('o');scope='Actual production full-image render/export methods only. Not interactive editor input-to-visible latency or frame pacing.';protocol=[ordered]@{blocks=$Blocks;samples_per_block=$Samples;warmups_per_process=$Warmups;order='Alternating by block, Zipshot first in block zero';affinity_mask='0xF, first four logical CPUs';priority='Normal';fixture_load_and_PNG_save_timed=$false;output_disposal_timed=$false;blur='App-native radius setting 15. Native export can reuse its whole-image effect cache; ShareX RenderOutputImage computes a cropped effect each call. Kernels differ, no pixel-equivalence claim.'};provenance=$provenance;runs=@($runs);summary=$summary}
$report|ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $out 'editor-export-results.json') -Encoding utf8
& python (Join-Path $PSScriptRoot 'editor_summarize.py') (Join-Path $out 'editor-export-results.json') | Out-Null
if($LASTEXITCODE){throw 'Raw export evidence validation failed'}
Write-Output (Join-Path $out 'editor-export-results.json')

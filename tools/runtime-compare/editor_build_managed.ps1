param([string]$ShareXDir='D:/Programs/ShareX', [string]$RuntimeStage='D:/Tools/ZipshotEditorBenchmark20261118',[string]$Harness='editor_sharex.cs')
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out=Join-Path $root 'build/runtime-compare/editor-managed'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$sdk=(dotnet --list-sdks | Select-Object -Last 1) -replace '^([^ ]+).*$','$1'
$csc=Join-Path "$env:ProgramFiles/dotnet/sdk/$sdk" 'Roslyn/bincore/csc.dll'
$names=@('System.Private.CoreLib','System.Private.Windows.Core','System.Runtime','System.Console','System.IO','System.Drawing.Common','System.Drawing.Primitives','System.Reflection','System.Runtime.Extensions','System.Runtime.InteropServices','System.Diagnostics.Process','System.ComponentModel.Primitives','System.Collections','System.Linq','System.Windows.Forms','System.Windows.Forms.Primitives','ShareX.ScreenCaptureLib','ShareX.HelpersLib')
$refs=$names|ForEach-Object {'/r:'+(Join-Path $ShareXDir ($_+'.dll'))}
$refs+=('/r:'+(Join-Path $ShareXDir 'System.Reflection.Primitives.dll'))
if($Harness -eq 'editor_ui_sharex22.cs'){
    $refs+=@('Avalonia.Base','Avalonia.Controls','Avalonia.Desktop','Avalonia.Themes.Fluent','Avalonia.Skia','ShareX.ImageEditor','SkiaSharp','System.Text.Json','System.Memory','System.ComponentModel','System.ObjectModel')|ForEach-Object {'/r:'+(Join-Path $ShareXDir ($_+'.dll'))}
}
& dotnet $csc /nologo /target:exe /optimize+ /nostdlib+ @refs "/out:$out/editor_sharex.dll" (Join-Path $PSScriptRoot $Harness)
if($LASTEXITCODE){throw 'Editor C# build failed'}
$ShareXDir=[IO.Path]::GetFullPath($ShareXDir)
$RuntimeStage=[IO.Path]::GetFullPath($RuntimeStage)
if($RuntimeStage.StartsWith($ShareXDir,[StringComparison]::OrdinalIgnoreCase)){throw 'Stage must be outside installed ShareX'}
$marker=Join-Path $RuntimeStage '.zipshot-editor-benchmark-runtime'
if((Test-Path -LiteralPath $RuntimeStage)-and -not(Test-Path -LiteralPath $marker)){throw 'Use a new empty task-owned stage'}
New-Item -ItemType Directory -Path $RuntimeStage -Force|Out-Null
'Task-owned editor benchmark stage, original installed runtime linked read-only in practice. Never modify runtime links.'|Set-Content -LiteralPath $marker
foreach($file in Get-ChildItem -LiteralPath $ShareXDir -File){
    if(($file.Extension -eq '.dll'-and $file.Name -ne 'ShareX.dll')-or $file.Name -in @('ShareX.exe','ShareX.runtimeconfig.json','ShareX.deps.json')){
        $dest=Join-Path $RuntimeStage $file.Name
        if(-not(Test-Path -LiteralPath $dest)){
            if([IO.Path]::GetPathRoot($dest)-eq [IO.Path]::GetPathRoot($file.FullName)){New-Item -ItemType HardLink -Path $dest -Value $file.FullName|Out-Null}
            else{Copy-Item -LiteralPath $file.FullName -Destination $dest}
        }
    }
}
Copy-Item -LiteralPath (Join-Path $out 'editor_sharex.dll') -Destination (Join-Path $RuntimeStage 'ShareX.dll') -Force
Write-Output $RuntimeStage

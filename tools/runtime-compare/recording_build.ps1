param([string]$ShareXDir='D:/Programs/ShareX',[string]$Stage='D:/ZipshotRuntimeCompare-20261118/recording-payload')
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Stage=[IO.Path]::GetFullPath($Stage)
if($Stage.StartsWith([IO.Path]::GetFullPath($ShareXDir),[StringComparison]::OrdinalIgnoreCase)){throw 'Stage must be separate from installed ShareX'}
New-Item -ItemType Directory -Path $Stage -Force | Out-Null
$runtime=Join-Path $Stage 'sharex-runtime'
New-Item -ItemType Directory -Path $runtime -Force | Out-Null
$sdk=(dotnet --list-sdks | Select-Object -Last 1) -replace '^([^ ]+).*$','$1'
$csc=Join-Path "$env:ProgramFiles/dotnet/sdk/$sdk" 'Roslyn/bincore/csc.dll'
$refNames=@('System.Private.CoreLib','System.Private.Windows.Core','System.Runtime','System.Console','System.IO','System.Drawing.Common','System.Drawing.Primitives','System.Reflection','System.Runtime.Extensions','System.Runtime.InteropServices','System.Diagnostics.Process','System.Threading','System.Threading.Thread','System.Windows.Forms','System.Windows.Forms.Primitives','System.ComponentModel.Primitives','System.ComponentModel','System.Collections')
$refs=$refNames | ForEach-Object {'/r:'+(Join-Path $ShareXDir ($_.ToString()+'.dll'))}
$harness=Join-Path $Stage 'recording_sharex.dll'
& dotnet $csc /nologo /target:exe /optimize+ /nostdlib+ @refs "/out:$harness" (Join-Path $PSScriptRoot 'recording_sharex.cs')
if($LASTEXITCODE){throw 'ShareX orchestration harness compile failed'}
foreach($file in (Get-ChildItem -LiteralPath $ShareXDir -File)) {
    if($file.Extension -eq '.dll' -or $file.Name -in @('ShareX.exe','ShareX.runtimeconfig.json','ShareX.deps.json')) {
        $name=if($file.Name -eq 'ShareX.dll'){'ShareX.original.dll'}else{$file.Name}
        $target=Join-Path $runtime $name
        if(-not(Test-Path -LiteralPath $target)){
            if([IO.Path]::GetPathRoot($target) -eq [IO.Path]::GetPathRoot($file.FullName)){New-Item -ItemType HardLink -Path $target -Value $file.FullName | Out-Null}
            else{Copy-Item -LiteralPath $file.FullName -Destination $target}
        }
    }
}
Copy-Item -LiteralPath $harness -Destination (Join-Path $runtime 'ShareX.dll') -Force
$frameworkCsc=Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$fixtureSource=Join-Path $PSScriptRoot 'recording_fixture.cs'
& $frameworkCsc /nologo /target:winexe /platform:x64 /r:System.Windows.Forms.dll /r:System.Drawing.dll ('/out:'+(Join-Path $Stage 'recording_fixture.exe')) $fixtureSource
if($LASTEXITCODE){throw 'Framework recording fixture compile failed'}
$ffmpeg=(Get-Command ffmpeg).Source;$ffprobe=(Get-Command ffprobe).Source
foreach($file in @($ffmpeg,$ffprobe,(Join-Path $root 'build/zipshot.exe'))){Copy-Item -LiteralPath $file -Destination (Join-Path $Stage ([IO.Path]::GetFileName($file))) -Force}
foreach($helper in (Get-ChildItem -LiteralPath $PSScriptRoot -Filter 'recording_*.ps1' -File)){
    if($helper.Name -ne 'recording_build.ps1'){Copy-Item -LiteralPath $helper.FullName -Destination $Stage -Force}
}
$manifest=[ordered]@{schema_version=1;purpose='Untouched real ShareX GUI controlled by reflection; recording stays in installed product DLLs';files=@()}
foreach($file in @((Join-Path $runtime 'ShareX.original.dll'),(Join-Path $runtime 'ShareX.ScreenCaptureLib.dll'),(Join-Path $Stage 'ffmpeg.exe'),(Join-Path $Stage 'ffprobe.exe'),(Join-Path $Stage 'zipshot.exe'),(Join-Path $Stage 'recording_fixture.exe'),$harness)){
    $item=Get-Item -LiteralPath $file;$manifest.files+=@{name=$item.Name;bytes=$item.Length;sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Stage 'recording_payload_manifest.json') -Encoding utf8
Write-Output $Stage

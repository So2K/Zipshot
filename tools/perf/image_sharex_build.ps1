param([string]$ShareXDir='D:/ZipshotRuntimeCompare-20261118/vendor/ShareX-22.4.0-portable-x64')
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$self=Get-Process -Id $PID
$self.PriorityClass='BelowNormal'
$self.ProcessorAffinity=15
& dotnet build (Join-Path $PSScriptRoot 'image_sharex.csproj') --configuration Release -m:1 /p:UseSharedCompilation=false /p:RestoreIgnoreFailedSources=true "/p:ShareXDir=$ShareXDir" "/p:BaseIntermediateOutputPath=$root/build/perf/images/sharex-obj/" "/p:MSBuildProjectExtensionsPath=$root/build/perf/images/sharex-obj/" --nologo
if($LASTEXITCODE){throw 'ShareX image harness build failed'}
$stage=Join-Path $root 'build/perf/images/sharex-runtime'
New-Item -ItemType Directory -Path $stage -Force|Out-Null
foreach($file in Get-ChildItem -LiteralPath $ShareXDir -File -Filter '*.dll'){
    $dest=Join-Path $stage $file.Name
    if(-not(Test-Path -LiteralPath $dest)){Copy-Item -LiteralPath $file.FullName -Destination $dest}
}
foreach($name in @('image_sharex.exe','image_sharex.dll')){
    Copy-Item -LiteralPath (Join-Path $root "build/perf/images/sharex-build/$name") -Destination (Join-Path $stage $name) -Force
}
# Use the vendor's exact self-contained .NET runtime/dependency configuration;
# original ShareX.dll remains untouched and is called as a library by our entry point.
Copy-Item -LiteralPath (Join-Path $ShareXDir 'ShareX.runtimeconfig.json') -Destination (Join-Path $stage 'image_sharex.runtimeconfig.json') -Force
Copy-Item -LiteralPath (Join-Path $ShareXDir 'ShareX.deps.json') -Destination (Join-Path $stage 'image_sharex.deps.json') -Force
Write-Output $stage

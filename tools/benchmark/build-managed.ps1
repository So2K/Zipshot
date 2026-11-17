param([string]$ShareXDir='D:/Programs/ShareX', [string]$RuntimeStage='')
$ErrorActionPreference='Stop'
$ShareXDir=[IO.Path]::GetFullPath($ShareXDir)
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out=Join-Path $root 'build/benchmark'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$sdk=(dotnet --list-sdks | Select-Object -Last 1) -replace '^([^ ]+).*$','$1'
$csc=Join-Path "$env:ProgramFiles/dotnet/sdk/$sdk" 'Roslyn/bincore/csc.dll'
$refs=@('System.Private.CoreLib.dll','System.Private.Windows.Core.dll','System.Runtime.dll','System.Console.dll','System.IO.dll','System.Drawing.Common.dll','System.Drawing.Primitives.dll','System.Reflection.dll','System.Runtime.Extensions.dll','System.Runtime.InteropServices.dll','System.Diagnostics.Process.dll') | ForEach-Object { '/r:'+(Join-Path $ShareXDir $_) }
foreach($name in @('capture')) {
    & dotnet $csc /nologo /target:exe /optimize+ /nostdlib+ @refs "/out:$out/sharex_$name.dll" (Join-Path $PSScriptRoot "sharex_$name.cs")
    if($LASTEXITCODE) { throw "C# build failed: $name" }
    '{"runtimeOptions":{"tfm":"net10.0","framework":{"name":"Microsoft.NETCore.App","version":"10.0.0"}}}' | Set-Content -LiteralPath (Join-Path $out "sharex_$name.runtimeconfig.json") -Encoding utf8
}
if($RuntimeStage) {
    # Use the original apphost and its self-contained runtime, without running ShareX's app.
    # Only our compiled harness is substituted in a separate, task-owned directory.
    $RuntimeStage=[IO.Path]::GetFullPath($RuntimeStage)
    if($RuntimeStage.StartsWith($ShareXDir,[StringComparison]::OrdinalIgnoreCase)) { throw 'RuntimeStage must be outside the installed ShareX folder.' }
    $marker=Join-Path $RuntimeStage '.zipshot-benchmark-runtime'
    if((Test-Path -LiteralPath $RuntimeStage) -and -not(Test-Path -LiteralPath $marker)) { throw 'Existing RuntimeStage lacks a Zipshot benchmark ownership marker. Use a new empty path.' }
    New-Item -ItemType Directory -Path $RuntimeStage -Force | Out-Null
    'Task-owned staged runtime for the Zipshot memory-only benchmark. Installed ShareX remains unchanged.' | Set-Content -LiteralPath $marker
    foreach($name in @('capture')) {
        $stage=Join-Path $RuntimeStage $name
        New-Item -ItemType Directory -Path $stage -Force | Out-Null
        foreach($file in (Get-ChildItem -LiteralPath $ShareXDir -File)) {
            if(($file.Extension -eq '.dll' -and $file.Name -ne 'ShareX.dll') -or $file.Name -in @('ShareX.exe','ShareX.runtimeconfig.json','ShareX.deps.json')) {
                $dest=Join-Path $stage $file.Name
                if(-not(Test-Path -LiteralPath $dest)) {
                    if([IO.Path]::GetPathRoot($dest) -eq [IO.Path]::GetPathRoot($file.FullName)) {
                        New-Item -ItemType HardLink -Path $dest -Value $file.FullName | Out-Null
                    } else { Copy-Item -LiteralPath $file.FullName -Destination $dest }
                }
            }
        }
        Copy-Item -LiteralPath (Join-Path $out "sharex_$name.dll") -Destination (Join-Path $stage 'ShareX.dll') -Force
    }
}

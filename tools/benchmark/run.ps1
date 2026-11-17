param(
    [string]$ShareXDir='D:/Programs/ShareX',
    [string]$RuntimeStage='D:/Tools/ZipshotBenchmark20261117',
    [string]$Output='docs/benchmark/capture-results.json',
    [ValidateRange(1,20)][int]$Blocks=3,
    [ValidateRange(0,1000)][int]$Warmups=3,
    [ValidateRange(1,1000)][int]$Samples=20
)
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location -LiteralPath $root
function Invoke-Bench([string]$exe,[string[]]$arguments) {
    $psi=[Diagnostics.ProcessStartInfo]::new($exe)
    foreach($arg in $arguments) { $psi.ArgumentList.Add($arg) }
    $psi.UseShellExecute=$false
    $psi.CreateNoWindow=$true
    $psi.RedirectStandardOutput=$true
    $psi.RedirectStandardError=$true
    $p=[Diagnostics.Process]::Start($psi)
    $stdout=$p.StandardOutput.ReadToEnd()
    $stderr=$p.StandardError.ReadToEnd()
    $p.WaitForExit()
    if($p.ExitCode) { throw "Benchmark exited $($p.ExitCode): $stderr" }
    return $stdout | ConvertFrom-Json
}
function Stats($values) {
    $v=@($values | Sort-Object)
    $n=$v.Count
    $median=if($n%2) { $v[[int][Math]::Floor($n/2)] } else { ($v[$n/2-1]+$v[$n/2])/2 }
    return [ordered]@{n=$n;median_ms=$median;p95_ms=$v[[Math]::Ceiling(.95*$n)-1];min_ms=$v[0];max_ms=$v[-1];mean_ms=($v | Measure-Object -Average).Average}
}
function Get-LfNormalizedSha256([string]$path) {
    $bytes=[IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $path).Path)
    $text=[Text.Encoding]::UTF8.GetString($bytes).Replace("`r`n","`n")
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($text)))
}
$runs=@()
for($block=0;$block -lt $Blocks;$block++) {
    $order=if($block%2) { @('sharex','zipshot') } else { @('zipshot','sharex') }
    foreach($engine in $order) {
        $exe=if($engine -eq 'zipshot') { Join-Path $root 'build/benchmark/zipshot_capture.exe' } else { Join-Path $RuntimeStage 'capture/ShareX.exe' }
        $args=if($engine -eq 'zipshot') { @("$Warmups","$Samples") } else { @($ShareXDir,"$Warmups","$Samples") }
        $r=Invoke-Bench $exe $args
        $runs += [ordered]@{block=$block;engine=$engine;result=$r}
    }
}
$zip=Stats (@($runs | Where-Object engine -eq 'zipshot' | ForEach-Object {$_.result.ms}) | ForEach-Object {$_})
$sx=Stats (@($runs | Where-Object engine -eq 'sharex' | ForEach-Object {$_.result.ms}) | ForEach-Object {$_})
$os=Get-CimInstance Win32_OperatingSystem
$record=[ordered]@{
    schema_version=1; utc=(Get-Date).ToUniversalTime().ToString('o');
    scope='Warm full-virtual-desktop capture API only. Cursor off. No encoding, disk, clipboard, hotkey dispatch or UI. All screen pixels stay in memory and are discarded.';
    privacy='No screenshots, window titles, desktop pixels or clipboard contents are persisted by these harnesses.';
    machine=[ordered]@{cpu=(Get-CimInstance Win32_Processor).Name;gpu=@((Get-CimInstance Win32_VideoController).Name);os=$os.Caption;build=$os.BuildNumber};
    protocol=[ordered]@{blocks=$Blocks;warmups_per_process=$Warmups;samples_per_block=$Samples;order='Alternating by block, Zipshot first in block 0';native_build='/O2 /GL /MT';sharex_runtime='Original self-contained apphost/runtime from installed ShareX, task-local staging';background_load='Normal user desktop and background applications remain running; no exclusive performance environment'};
    provenance=[ordered]@{zipshot_base=(git rev-parse HEAD);zipshot_capture_sha256=(Get-FileHash src/capture.cpp -Algorithm SHA256).Hash;zipshot_capture_normalized_sha256=(Get-LfNormalizedSha256 'src/capture.cpp');native_harness_sha256=(Get-FileHash build/benchmark/zipshot_capture.exe -Algorithm SHA256).Hash;sharex_version=(Get-Item (Join-Path $ShareXDir 'ShareX.dll')).VersionInfo.ProductVersion;sharex_capture_sha256=(Get-FileHash (Join-Path $ShareXDir 'ShareX.ScreenCaptureLib.dll') -Algorithm SHA256).Hash};
    summary=[ordered]@{zipshot=$zip;sharex=$sx;median_speedup=$sx.median_ms/$zip.median_ms};
    runs=$runs;
    limitations=@('Installed ShareX 18.0.1 is compared, not the latest ShareX release.','API latency is not user-perceived hotkey-to-overlay or capture-to-save latency.','Results are machine and workload dependent; not a universal product speed claim.','Capture output dimensions and non-null return are checked; no screen-content validation or published image comparison is performed.','No evidence supports a 1000x speed claim.')
}
$record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Output -Encoding utf8
$record.summary | ConvertTo-Json -Depth 5

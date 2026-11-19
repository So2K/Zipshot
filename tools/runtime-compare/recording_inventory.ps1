param([string]$Output='docs/runtime-comparison/recording_capabilities.json')
$ErrorActionPreference='Stop'
$ffmpeg=(Get-Command ffmpeg).Source;$ffprobe=(Get-Command ffprobe).Source
function Invoke-Inventory([string[]]$Arguments){@(& $ffmpeg @Arguments 2>&1 | ForEach-Object {$_.ToString()})}
$version=Invoke-Inventory @('-version')
$encoders=Invoke-Inventory @('-hide_banner','-encoders')
$filters=Invoke-Inventory @('-hide_banner','-filters')
$devices=Invoke-Inventory @('-hide_banner','-devices')
$report=[ordered]@{
 schema_version=1;scope='Capability inventory only. Listing an encoder is not proof it initializes in Windows Sandbox.'
 ffmpeg=@{version=$version[0];sha256=(Get-FileHash -LiteralPath $ffmpeg -Algorithm SHA256).Hash.ToLowerInvariant();bytes=(Get-Item -LiteralPath $ffmpeg).Length}
 ffprobe=@{sha256=(Get-FileHash -LiteralPath $ffprobe -Algorithm SHA256).Hash.ToLowerInvariant();bytes=(Get-Item -LiteralPath $ffprobe).Length}
 encoders=@($encoders | Where-Object {$_ -match '\b(libx264|libx264rgb|h264_nvenc|gif)\b'})
 filters=@($filters | Where-Object {$_ -match '\b(ddagrab|palettegen|paletteuse|testsrc2)\b'})
 devices=@($devices | Where-Object {$_ -match '\b(gdigrab|lavfi|dshow)\b'})
 zipshot_contract=@{source='src/recorder.cpp';video_defaults='ddagrab+NVENC, fallback ddagrab+x264, fallback gdigrab+x264';gif='lossless libx264rgb MKV segments then palettegen/paletteuse';selector_note='Current release has preset and CRF settings but no encoder/backend selector.'}
 sharex_legacy_contract=@{version='18.0.1';source='Installed ShareX.ScreenCaptureLib.dll metadata and decompiled ScreenRecordingOptions/ScreenRecorder';video_defaults='gdigrab and libx264, source and codec configurable; supports actual custom FFmpeg commands';gif='libx264 lossless intermediary and FFmpeg palettegen/paletteuse';scope_note='Do not generalize the 18.0.1 pipeline to newer ShareX releases.'}
}
$parent=Split-Path $Output -Parent;if($parent){New-Item -ItemType Directory -Path $parent -Force | Out-Null}
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Output -Encoding utf8
Write-Output $Output

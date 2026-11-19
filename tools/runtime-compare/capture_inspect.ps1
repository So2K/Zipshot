param([Parameter(Mandatory=$true)][string]$OutputPrefix)
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
if($env:USERNAME -ne 'WDAGUtilityAccount' -or -not(Test-Path 'C:/ZipshotLab/input/.isolation-approved.json')){throw 'Guest only; refusing host pixels.'}
if(-not([IO.Path]::GetFullPath($OutputPrefix).StartsWith('C:\ZipshotLab\',[StringComparison]::OrdinalIgnoreCase))){throw 'Output must stay inside the task guest workspace.'}
Add-Type -AssemblyName System.Drawing
if(-not('CaptureInspectNative' -as [type])) {
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public static class CaptureInspectNative {
 public delegate bool CB(IntPtr h,IntPtr p);
 [StructLayout(LayoutKind.Sequential)]public struct R {public int Left,Top,Right,Bottom;}
 [DllImport("user32.dll")]public static extern int GetSystemMetrics(int i);
 [DllImport("user32.dll")]public static extern bool SetProcessDpiAwarenessContext(IntPtr p);
 [DllImport("user32.dll")]public static extern bool EnumWindows(CB c,IntPtr p);
 [DllImport("user32.dll")]public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out R r);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
}
'@
}
[CaptureInspectNative]::SetProcessDpiAwarenessContext([IntPtr](-4))|Out-Null
$x=[CaptureInspectNative]::GetSystemMetrics(76);$y=[CaptureInspectNative]::GetSystemMetrics(77)
$width=[CaptureInspectNative]::GetSystemMetrics(78);$height=[CaptureInspectNative]::GetSystemMetrics(79)
$image=New-Object Drawing.Bitmap($width,$height)
$graphics=[Drawing.Graphics]::FromImage($image)
try {$graphics.CopyFromScreen($x,$y,0,0,$image.Size,[Drawing.CopyPixelOperation]::SourceCopy);$image.Save(($OutputPrefix+'.png'),[Drawing.Imaging.ImageFormat]::Png)}finally{$graphics.Dispose();$image.Dispose()}
$windows=[Collections.Generic.List[object]]::new()
[CaptureInspectNative]::EnumWindows({param($hwnd,$parameter)
 if([CaptureInspectNative]::IsWindowVisible($hwnd)){
  [uint32]$ownerPid=0;[CaptureInspectNative]::GetWindowThreadProcessId($hwnd,[ref]$ownerPid)|Out-Null
  $rect=New-Object CaptureInspectNative+R;[CaptureInspectNative]::GetWindowRect($hwnd,[ref]$rect)|Out-Null
  $class=New-Object Text.StringBuilder(256);[CaptureInspectNative]::GetClassName($hwnd,$class,256)|Out-Null
  $windows.Add([pscustomobject]@{pid=$ownerPid;class=$class.ToString();x=$rect.Left;y=$rect.Top;width=$rect.Right-$rect.Left;height=$rect.Bottom-$rect.Top})
 }
 return $true
},[IntPtr]::Zero)|Out-Null
[ordered]@{guest=$env:USERNAME;session=[Diagnostics.Process]::GetCurrentProcess().SessionId;utc=[DateTime]::UtcNow.ToString('o');display=@{x=$x;y=$y;width=$width;height=$height};windows=@($windows);processes=@(Get-Process | Select-Object Id,ProcessName,WorkingSet64,PrivateMemorySize64)}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath ($OutputPrefix+'.json') -Encoding UTF8

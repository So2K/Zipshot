param([string]$OutputPrefix='C:/ZipshotLab/output/inspection')
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount' -or -not(Test-Path 'C:/ZipshotLab/input/.isolation-approved.json')){throw 'Guest only'}
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;using System.Text;using System.Drawing;using System.Drawing.Imaging;using System.Runtime.InteropServices;using System.Collections.Generic;
public static class RuntimeInspect {
 public delegate bool EnumCallback(IntPtr h,IntPtr data);
 [DllImport("user32.dll")]static extern bool EnumWindows(EnumCallback c,IntPtr p);
 [DllImport("user32.dll")]static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")]static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetWindowText(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassName(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")]static extern int GetSystemMetrics(int i);
 [DllImport("user32.dll")]static extern bool SetProcessDpiAwarenessContext(IntPtr c);
 [StructLayout(LayoutKind.Sequential)]public struct Rect{public int left,top,right,bottom;}
 [DllImport("user32.dll")]static extern bool GetWindowRect(IntPtr h,out Rect r);
 public class Window{public long hwnd;public uint pid;public string title,cls;public Rect rect;}
 public static Window[] Windows(){var all=new List<Window>();EnumWindows((h,p)=>{if(IsWindowVisible(h)){uint pid;GetWindowThreadProcessId(h,out pid);var t=new StringBuilder(512);var c=new StringBuilder(128);GetWindowText(h,t,512);GetClassName(h,c,128);Rect r;GetWindowRect(h,out r);all.Add(new Window{hwnd=h.ToInt64(),pid=pid,title=t.ToString(),cls=c.ToString(),rect=r});}return true;},IntPtr.Zero);return all.ToArray();}
 public static void Capture(string path){SetProcessDpiAwarenessContext(new IntPtr(-4));int w=GetSystemMetrics(0),h=GetSystemMetrics(1);using(var b=new Bitmap(w,h,PixelFormat.Format32bppArgb)){using(var g=Graphics.FromImage(b))g.CopyFromScreen(0,0,0,0,new Size(w,h));b.Save(path,ImageFormat.Png);}}
}
'@
$parent=Split-Path $OutputPrefix -Parent;New-Item -ItemType Directory -Path $parent -Force|Out-Null
[RuntimeInspect]::Capture($OutputPrefix+'.png')
[ordered]@{utc=[DateTime]::UtcNow.ToString('o');windows=@([RuntimeInspect]::Windows());processes=@(Get-Process | Where-Object {$_.ProcessName -match 'ShareX|zipshot|editor_ui|ffmpeg'} | Select-Object Id,ProcessName,WorkingSet64,PrivateMemorySize64,CPU)} | ConvertTo-Json -Depth 8 | Set-Content ($OutputPrefix+'.json') -Encoding UTF8
Write-Output ('Saved '+$OutputPrefix)

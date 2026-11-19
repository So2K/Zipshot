# Untouched stock ShareX CLI in a writable disposable-guest copy, no mouse/key input.
param([string]$Fixture='C:/ZipshotLab/input/fixtures/editor-fixture-1920x1080.png',[string]$OutputRoot='C:/ZipshotLab/output/editor-stock22-probes')
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount' -or [Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0 -or $env:ZIPSHOT_RUNTIME_ISOLATED -ne '1' -or -not(Test-Path -LiteralPath 'C:/ZipshotLab/input/.isolation-approved.json')){throw 'Approved guest required'}
$run=Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss');New-Item -ItemType Directory -Path $run -Force|Out-Null
$report=[ordered]@{schema_version=1;status='running';utc_start=[DateTime]::UtcNow.ToString('o');method='Unmodified ShareX22 -ImageEditor CLI, actual displayed window capture';no_input_injected=$true;fixture_sha256=(Get-FileHash -LiteralPath $Fixture).Hash}
$process=$null;$stdout=$null;$stderr=$null
Add-Type @'
using System;using System.Text;using System.Collections.Generic;using System.Runtime.InteropServices;
public static class EditorStockWindows {
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [StructLayout(LayoutKind.Sequential)]public struct RECT{public int left,top,right,bottom;}
 public class Window{public long hwnd;public string title,cls;public int left,top,right,bottom;}
 [DllImport("user32.dll")]static extern bool EnumWindows(EnumProc c,IntPtr l);
 [DllImport("user32.dll")]static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll")]static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetWindowText(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassName(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")]static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")]public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int hh,uint f);
 [DllImport("user32.dll")]static extern bool SetProcessDpiAwarenessContext(IntPtr c);
 [DllImport("user32.dll")]public static extern uint GetDpiForSystem();
 public static void Dpi(){SetProcessDpiAwarenessContext(new IntPtr(-4));}
 public static Window[] List(uint pid){var rows=new List<Window>();EnumWindows((h,l)=>{uint owner;GetWindowThreadProcessId(h,out owner);if(owner!=pid||!IsWindowVisible(h))return true;var title=new StringBuilder(512);var cls=new StringBuilder(128);RECT r;GetWindowText(h,title,512);GetClassName(h,cls,128);GetWindowRect(h,out r);rows.Add(new Window{hwnd=h.ToInt64(),title=title.ToString(),cls=cls.ToString(),left=r.left,top=r.top,right=r.right,bottom=r.bottom});return true;},IntPtr.Zero);return rows.ToArray();}
}
'@
[EditorStockWindows]::Dpi()
try{
    $original='C:/ZipshotLab/input/sharex22';$appRoot='C:/ZipshotLab/work/editor-stock22-app'
    if(-not(Test-Path -LiteralPath $original)){throw 'Original stock ShareX22 payload missing'}
    if(-not(Test-Path -LiteralPath (Join-Path $appRoot '.editor-owned-copy'))){
        if(Test-Path -LiteralPath $appRoot){throw 'Existing app copy lacks ownership marker'}
        Copy-Item -LiteralPath $original -Destination $appRoot -Recurse
        'Task-owned writable copy of stock ShareX22, binaries unchanged'|Set-Content -LiteralPath (Join-Path $appRoot '.editor-owned-copy')
    }
    foreach($name in @('ShareX.exe','ShareX.dll','ShareX.ImageEditor.dll')){if((Get-FileHash -LiteralPath (Join-Path $original $name)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $appRoot $name)).Hash){throw "Stock binary changed $name"}}
    $configRoot=Join-Path $appRoot 'ShareX';New-Item -ItemType Directory -Path $configRoot -Force|Out-Null
    $dpi=[EditorStockWindows]::GetDpiForSystem();$report.system_dpi=$dpi
    $options=@{Thickness=4;BorderColorHex='#FFFF0000';Shadow=$false;BlurStrength=15;ZoomToFitOnOpen=$true;ShowExitConfirmation=$false;AutoCopyImageToClipboard=$false;AutoCloseEditorOnTask=$false;ShowNotifications=$false;RememberWindowState=$true;IsWindowMaximized=$false;WindowWidth=(1280.0*96/$dpi);WindowHeight=(900.0*96/$dpi);LastUsedAnnotationTool='Select';BackgroundMargin=0;BackgroundPadding=0;BackgroundSmartPadding=$false;BackgroundRoundedCorner=0;BackgroundShadowRadius=0}
    # Verified from shipped ShareX22 metadata, not a guessed legacy AnnotationOptions key.
    $config=@{ShowStartScreen=$false;ShowIconPackPrompt=$false;FirstTimeMinimizeToTray=$false;AutoCheckUpdate=$false;ShowTray=$true;ShowClipboardContentViewer=$false;DisableUpload=$true;Language='English';DefaultTaskSettings=@{AfterCaptureJob='None';AfterUploadJob='None';ToolsSettings=@{ImageEditorOptions=$options};GeneralSettings=@{PlaySoundAfterCapture=$false;PlaySoundAfterUpload=$false;PlaySoundAfterAction=$false}}}
    $config|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $configRoot 'ApplicationConfig.json') -Encoding UTF8
    $workFixture=Join-Path $run 'working-fixture.png';Copy-Item -LiteralPath $Fixture -Destination $workFixture
    $info=New-Object Diagnostics.ProcessStartInfo;$info.FileName=Join-Path $appRoot 'ShareX.exe';$info.WorkingDirectory=$appRoot;$info.Arguments='-portable -multi -silent -nohotkeys -ImageEditor "'+$workFixture+'"'
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $process=[Diagnostics.Process]::Start($info);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();$report.pid=$process.Id
    $deadline=[DateTime]::UtcNow.AddSeconds(45);$window=$null
    while($null -eq $window){
        if($process.HasExited){throw "Stock app exited early $($process.ExitCode)"}
        $windows=@([EditorStockWindows]::List([uint32]$process.Id));$report.observed_windows=$windows
        $window=@($windows|Where-Object {($_.right-$_.left) -gt 600 -and ($_.bottom-$_.top) -gt 400 -and ($_.title -match 'working-fixture|Image.Editor|Image editor' -or $_.cls -match 'Avalonia')}|Select-Object -First 1)[0]
        if([DateTime]::UtcNow -gt $deadline){throw 'No actual stock editor window before deadline'}
        if($null -eq $window){Start-Sleep -Milliseconds 200}
    }
    [EditorStockWindows]::SetWindowPos([IntPtr]::new($window.hwnd),[IntPtr]::new(-1),40,40,0,0,0x41)|Out-Null
    Start-Sleep -Milliseconds 1800
    $window=@([EditorStockWindows]::List([uint32]$process.Id)|Where-Object hwnd -eq $window.hwnd)[0];$report.window=$window
    $w=($window.right-$window.left+1)-band(-bnot 1);$h=($window.bottom-$window.top+1)-band(-bnot 1)
    $captureInfo=New-Object Diagnostics.ProcessStartInfo;$captureInfo.FileName='C:/ZipshotLab/input/ffmpeg.exe';$captureInfo.Arguments='-hide_banner -loglevel error -y -f gdigrab -draw_mouse 0 -offset_x '+$window.left+' -offset_y '+$window.top+' -video_size '+$w+'x'+$h+' -i desktop -frames:v 1 "'+(Join-Path $run 'stock-editor-window.png')+'"'
    $captureInfo.UseShellExecute=$false;$captureInfo.CreateNoWindow=$true;$captureInfo.RedirectStandardError=$true
    $capture=[Diagnostics.Process]::Start($captureInfo);$captureError=$capture.StandardError.ReadToEndAsync()
    if(-not $capture.WaitForExit(15000)){throw 'Actual stock editor screenshot deadline exceeded'}
    $captureError.Result|Set-Content -LiteralPath (Join-Path $run 'capture-stderr.txt')
    if($capture.ExitCode -ne 0 -or -not(Test-Path -LiteralPath (Join-Path $run 'stock-editor-window.png'))){throw 'Actual stock displayed-window capture failed'}
    $report.stock_binary_sha256=(Get-FileHash -LiteralPath (Join-Path $appRoot 'ShareX.dll')).Hash
    $report.editor_binary_sha256=(Get-FileHash -LiteralPath (Join-Path $appRoot 'ShareX.ImageEditor.dll')).Hash
    $report.image_sha256=(Get-FileHash -LiteralPath (Join-Path $run 'stock-editor-window.png')).Hash
    $report.status='captured_pending_visual_review'
}catch{$report.status='fail';$report.error=$_.Exception.Message}
finally{
    if($null -ne $process -and -not $process.HasExited){$process.CloseMainWindow()|Out-Null;if(-not $process.WaitForExit(2000)){$process.Kill();$process.WaitForExit()}}
    if($null -ne $process){$stdout.Result|Set-Content -LiteralPath (Join-Path $run 'stdout.txt');$stderr.Result|Set-Content -LiteralPath (Join-Path $run 'stderr.txt')}
    $report.utc_end=[DateTime]::UtcNow.ToString('o');$path=Join-Path $run 'editor-stock22-probe-result.json';$report|ConvertTo-Json -Depth 15|Set-Content -LiteralPath $path -Encoding UTF8;Write-Output $path
}

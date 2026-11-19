# Real input and real-time window recording, strictly inside the approved disposable guest.
param([ValidateSet('native','sharex22')][string]$Engine='native',[ValidateSet('arrow','freehand','blur','zoom','undo','text','pixelate','colorpicker')][string]$Case='arrow',[string]$Fixture='C:/ZipshotLab/input/fixtures/editor-fixture-1920x1080.png',[string]$OutputRoot='C:/ZipshotLab/output/editor-interactions',[string]$NativeExe='C:/ZipshotLab/input/editor_ui_native_r3.exe',[string]$ShareXExe='C:/ZipshotLab/input/editor22_r3/ShareX.exe')
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount' -or [Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0 -or $env:ZIPSHOT_RUNTIME_ISOLATED -ne '1' -or -not(Test-Path -LiteralPath 'C:/ZipshotLab/input/.isolation-approved.json')){throw 'Refusing input or capture outside the approved isolated guest'}
$run=Join-Path $OutputRoot ($Engine+'-'+$Case+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $run -Force|Out-Null
$prefix=Join-Path $run 'editor'
$report=[ordered]@{schema_version=1;status='running';engine=$Engine;case=$Case;utc_start=[DateTime]::UtcNow.ToString('o');fixture=$Fixture;fixture_sha256=(Get-FileHash -LiteralPath $Fixture).Hash;scope='Real guest input and captured displayed pixels. Paint CPU events are separate; visible latency requires offline frame analysis.';output_prefix=$prefix}
$app=$null;$capture=$null;$appStdout=$null;$appStderr=$null;$captureStderr=$null
$samples=New-Object 'Collections.Generic.List[object]'
$phases=New-Object 'Collections.Generic.List[object]'
Add-Type -ReferencedAssemblies 'System.Drawing','System.Core' @'
using System;using System.Diagnostics;using System.Runtime.InteropServices;using System.Threading;using System.Collections.Generic;using System.Text;using System.Drawing;using System.Drawing.Imaging;
public static class EditorGuestDrive {
 [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public UNION value; }
 [StructLayout(LayoutKind.Explicit)] struct UNION { [FieldOffset(0)] public MOUSE mouse; [FieldOffset(0)] public KEY key; }
 [StructLayout(LayoutKind.Sequential)] struct MOUSE { public int dx,dy;public uint data,flags,time;public UIntPtr extra; }
 [StructLayout(LayoutKind.Sequential)] struct KEY { public ushort vk,scan;public uint flags,time;public UIntPtr extra; }
 [DllImport("user32.dll",SetLastError=true)]static extern uint SendInput(uint count,INPUT[] inputs,int size);
 [DllImport("user32.dll")]static extern int GetSystemMetrics(int index);
 [DllImport("user32.dll")]public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
 [DllImport("user32.dll")]public static extern IntPtr WindowFromPoint(POINT point);
 [DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr window);
 [DllImport("user32.dll")]public static extern bool BringWindowToTop(IntPtr window);
 [DllImport("user32.dll")]public static extern bool ShowWindow(IntPtr window,int command);
 [DllImport("user32.dll")]static extern bool SetProcessDpiAwarenessContext(IntPtr value);
 [DllImport("user32.dll")]public static extern uint GetDpiForSystem();
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]static extern bool EnumWindows(EnumProc callback,IntPtr data);
 [DllImport("user32.dll")]static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetWindowText(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassName(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")]static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")]public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int width,int height,uint flags);
 [StructLayout(LayoutKind.Sequential)]struct RECT{public int left,top,right,bottom;}
 public class Window{public long hwnd;public string title,cls;public int left,top,right,bottom;}
 public static Window[] Windows(uint pid){var rows=new List<Window>();EnumWindows((h,l)=>{uint owner;GetWindowThreadProcessId(h,out owner);if(owner!=pid||!IsWindowVisible(h))return true;var title=new StringBuilder(512);var cls=new StringBuilder(128);RECT r;GetWindowText(h,title,512);GetClassName(h,cls,128);GetWindowRect(h,out r);rows.Add(new Window{hwnd=h.ToInt64(),title=title.ToString(),cls=cls.ToString(),left=r.left,top=r.top,right=r.right,bottom=r.bottom});return true;},IntPtr.Zero);return rows.ToArray();}
 public class Sheet{public int x,y,width,height,area;}
 static bool SheetColor(int p){int r=(p>>16)&255,g=(p>>8)&255,b=p&255;return r>=226&&r<=241&&g>=230&&g<=246&&b>=235&&b<=250;}
 public static Sheet FindSheet(string path){using(var loaded=new Bitmap(path))using(var image=new Bitmap(loaded.Width,loaded.Height,PixelFormat.Format32bppArgb)){
  using(var g=Graphics.FromImage(image))g.DrawImageUnscaled(loaded,0,0);
  int w=image.Width,h=image.Height;var pixels=new int[w*h];var bits=image.LockBits(new Rectangle(0,0,w,h),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);for(int y=0;y<h;y++)Marshal.Copy(IntPtr.Add(bits.Scan0,y*bits.Stride),pixels,y*w,w);image.UnlockBits(bits);
  var seen=new byte[pixels.Length];var queue=new int[pixels.Length];var best=new Sheet();
  for(int start=0;start<pixels.Length;start++){if(seen[start]!=0||!SheetColor(pixels[start]))continue;int head=0,tail=1;queue[0]=start;seen[start]=1;int minX=w,minY=h,maxX=0,maxY=0;
   while(head<tail){int index=queue[head++],x=index%w,y=index/w;minX=Math.Min(minX,x);maxX=Math.Max(maxX,x);minY=Math.Min(minY,y);maxY=Math.Max(maxY,y);int[] neighbors=new[]{x>0?index-1:-1,x<w-1?index+1:-1,y>0?index-w:-1,y<h-1?index+w:-1};foreach(int next in neighbors)if(next>=0&&seen[next]==0&&SheetColor(pixels[next])){seen[next]=1;queue[tail++]=next;}}
   if(tail>best.area)best=new Sheet{x=minX,y=minY,width=maxX-minX+1,height=maxY-minY+1,area=tail};
  }return best;
 }}
 [DllImport("kernel32.dll")]static extern void GetSystemTimePreciseAsFileTime(out long value);
 [DllImport("winmm.dll")]public static extern uint timeBeginPeriod(uint value);
 [DllImport("winmm.dll")]public static extern uint timeEndPeriod(uint value);
 [DllImport("user32.dll",SetLastError=true)]public static extern IntPtr SendMessageTimeoutW(IntPtr window,uint message,IntPtr w,IntPtr l,uint flags,uint timeout,out UIntPtr result);
 [StructLayout(LayoutKind.Sequential)]public struct POINT {public int x,y;public POINT(int xx,int yy){x=xx;y=yy;}}
 public static void Dpi(){SetProcessDpiAwarenessContext(new IntPtr(-4));}
 public static long Qpc(){return Stopwatch.GetTimestamp();}
 public static long Frequency(){return Stopwatch.Frequency;}
 public static long FileTime(){long value;GetSystemTimePreciseAsFileTime(out value);return value;}
 static void Send(INPUT input){if(SendInput(1,new INPUT[]{input},Marshal.SizeOf(typeof(INPUT)))!=1)throw new Exception("Guest SendInput failed "+Marshal.GetLastWin32Error());}
 public static void Move(int x,int y){int left=GetSystemMetrics(76),top=GetSystemMetrics(77),width=GetSystemMetrics(78),height=GetSystemMetrics(79);Send(new INPUT{type=0,value=new UNION{mouse=new MOUSE{dx=(int)Math.Round((x-left)*65535.0/(width-1)),dy=(int)Math.Round((y-top)*65535.0/(height-1)),flags=0xC001}}});}
 public static void Button(bool down){Send(new INPUT{type=0,value=new UNION{mouse=new MOUSE{flags=down?2u:4u}}});}
 public static void Key(int code,bool down){Send(new INPUT{type=1,value=new UNION{key=new KEY{vk=(ushort)code,flags=down?0u:2u}}});}
 public static void Character(char value){Send(new INPUT{type=1,value=new UNION{key=new KEY{scan=value,flags=4}}});Send(new INPUT{type=1,value=new UNION{key=new KEY{scan=value,flags=6}}});}
 public static POINT RedControl(string path){using(var bitmap=new Bitmap(path)){int w=bitmap.Width,h=Math.Min(bitmap.Height/3,240),bestArea=0,bx=-1,by=-1;var red=new bool[w*h];var seen=new bool[w*h];var queue=new int[w*h];for(int y=20;y<h;y++)for(int x=20;x<w-20;x++){var c=bitmap.GetPixel(x,y);red[y*w+x]=c.R>200&&c.G<60&&c.B<60;}for(int start=0;start<red.Length;start++){if(!red[start]||seen[start])continue;int head=0,tail=1,minX=w,minY=h,maxX=0,maxY=0;queue[0]=start;seen[start]=true;while(head<tail){int i=queue[head++],x=i%w,y=i/w;minX=Math.Min(minX,x);maxX=Math.Max(maxX,x);minY=Math.Min(minY,y);maxY=Math.Max(maxY,y);int[] nexts=new[]{x>0?i-1:-1,x<w-1?i+1:-1,y>0?i-w:-1,y<h-1?i+w:-1};foreach(int next in nexts)if(next>=0&&red[next]&&!seen[next]){seen[next]=true;queue[tail++]=next;}}if(maxX-minX>=12&&maxY-minY>=12&&tail>bestArea){bestArea=tail;bx=(minX+maxX)/2;by=(minY+maxY)/2;}}if(bx<0)throw new Exception("No observed red color control");return new POINT(bx,by);}}
 public static void Wheel(int delta){Send(new INPUT{type=0,value=new UNION{mouse=new MOUSE{data=unchecked((uint)delta),flags=0x800}}});}
 public static void Wait(long target){while(true){double ms=(target-Stopwatch.GetTimestamp())*1000.0/Stopwatch.Frequency;if(ms<=0)return;if(ms>2)Thread.Sleep((int)ms-1);else Thread.SpinWait(30);}}
}
'@
[EditorGuestDrive]::Dpi()
$frequency=[EditorGuestDrive]::Frequency()
function Clock-Anchor{
    $before=[EditorGuestDrive]::Qpc();$fileTime=[EditorGuestDrive]::FileTime();$after=[EditorGuestDrive]::Qpc()
    return [ordered]@{qpc_before=$before;qpc_after=$after;filetime_utc_100ns=$fileTime;utc=[DateTime]::FromFileTimeUtc($fileTime).ToString('o');qpc_frequency=$frequency}
}
function Start-Actor([string]$Exe,[string[]]$ActorArguments,[bool]$InputPipe=$false){
    $info=New-Object Diagnostics.ProcessStartInfo;$info.FileName=$Exe;$info.Arguments=(@($ActorArguments|ForEach-Object {'"'+$_+'"'})-join ' ')
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.RedirectStandardInput=$InputPipe
    return [Diagnostics.Process]::Start($info)
}
function Image-Point([double]$X,[double]$Y){
    if($Engine -eq 'native'){
        return @{x=[int][Math]::Round($ready.client_origin[0]+$ready.image_to_client.off_x+$X*$ready.image_to_client.scale);y=[int][Math]::Round($ready.client_origin[1]+$ready.image_to_client.off_y+$Y*$ready.image_to_client.scale)}
    }
    $origin=$ready.source_origin;$axisX=$ready.source_x1000;$axisY=$ready.source_y1000
    return @{x=[int][Math]::Round($origin.x+$X*($axisX.x-$origin.x)/1000+$Y*($axisY.x-$origin.x)/1000);y=[int][Math]::Round($origin.y+$X*($axisX.y-$origin.y)/1000+$Y*($axisY.y-$origin.y)/1000)}
}
function Input-Key([int]$Code,[bool]$Down,[string]$Phase){
    $begin=[EditorGuestDrive]::Qpc();[EditorGuestDrive]::Key($Code,$Down);$end=[EditorGuestDrive]::Qpc()
    $samples.Add([ordered]@{kind='key';code=$Code;down=$Down;phase=$Phase;qpc_before=$begin;qpc_after=$end})
}
function Input-Move([int]$X,[int]$Y,[string]$Phase,[int]$Index=-1,[long]$Scheduled=0){
    $begin=[EditorGuestDrive]::Qpc();[EditorGuestDrive]::Move($X,$Y);$end=[EditorGuestDrive]::Qpc()
    $samples.Add([ordered]@{kind='move';x=$X;y=$Y;phase=$Phase;index=$Index;scheduled_qpc=$Scheduled;qpc_before=$begin;qpc_after=$end})
}
function Input-Button([bool]$Down,[string]$Phase){
    $begin=[EditorGuestDrive]::Qpc();[EditorGuestDrive]::Button($Down);$end=[EditorGuestDrive]::Qpc()
    $samples.Add([ordered]@{kind='button';down=$Down;phase=$Phase;qpc_before=$begin;qpc_after=$end})
}
function Shortcut([int]$Code,[string]$Phase){Input-Key 17 $true $Phase;Input-Key $Code $true $Phase;Input-Key $Code $false $Phase;Input-Key 17 $false $Phase}
function Draw-Stroke([string]$Kind,[string]$Phase){
    $points=New-Object 'Collections.Generic.List[object]'
    for($index=0;$index -lt 120;$index++){
        $t=$index/119.0
        if($Kind -eq 'freehand'){$x=$imageWidth*(.15+.7*$t);$y=$imageHeight*(.5+.18*[Math]::Sin($index*6*[Math]::PI/119))}
        elseif($Kind -in @('blur','pixelate')){$x=$imageWidth*(.25+.5*$t);$y=$imageHeight*(.3+.4*$t)}
        else{$x=$imageWidth*(.2+.6*$t);$y=$imageHeight*(.75-.5*$t)}
        $mapped=Image-Point $x $y;$points.Add(@{x=$mapped.x;y=$mapped.y;source_x=$x;source_y=$y})
    }
    Input-Move $points[0].x $points[0].y $Phase 0
    $start=[EditorGuestDrive]::Qpc();Input-Button $true $Phase
    for($index=1;$index -lt 120;$index++){
        $target=$start+[long]($frequency*2.0*$index/119)
        [EditorGuestDrive]::Wait($target);Input-Move $points[$index].x $points[$index].y $Phase $index $target
    }
    Input-Button $false $Phase
    $phases.Add([ordered]@{name=$Phase;kind=$Kind;qpc_start=$start;qpc_end=[EditorGuestDrive]::Qpc();points=$points.ToArray();nominal_duration_ms=2000;point_count=120})
}
function Native-Message([int]$Message){$reply=[UIntPtr]::Zero;$sent=[EditorGuestDrive]::SendMessageTimeoutW($hwnd,$Message,[IntPtr]::Zero,[IntPtr]::Zero,2,20000,[ref]$reply);if($sent -eq [IntPtr]::Zero -or $reply -eq [UIntPtr]::Zero){throw 'Owned native helper command failed'}}
function Capture-Still([string]$Path,[int]$Left,[int]$Top,[int]$Width,[int]$Height){
    $still=Start-Actor 'C:/ZipshotLab/input/ffmpeg.exe' @('-hide_banner','-loglevel','error','-y','-f','gdigrab','-draw_mouse','0','-offset_x',$Left,'-offset_y',$Top,'-video_size',($Width.ToString()+'x'+$Height),'-i','desktop','-frames:v','1',$Path)
    $errors=$still.StandardError.ReadToEndAsync();$unused=$still.StandardOutput.ReadToEndAsync()
    if(-not $still.WaitForExit(15000)){throw 'Actual viewport screenshot timeout'}
    if($still.ExitCode -ne 0){throw ('Viewport capture failed '+$errors.Result)}
}
try{
    $exe=if($Engine -eq 'native'){$NativeExe}else{'C:/ZipshotLab/work/editor-stock22-app/ShareX.exe'}
    $report.clock_before_app=Clock-Anchor
    if($Engine -eq 'native'){
        $app=Start-Actor $exe @($Fixture,$prefix,'1280','900','60000');$appStdout=$app.StandardOutput.ReadToEndAsync();$appStderr=$app.StandardError.ReadToEndAsync()
        $deadline=[DateTime]::UtcNow.AddSeconds(40)
        while(-not(Test-Path -LiteralPath ($prefix+'-ready.json'))){if($app.HasExited){throw "Editor exited before ready $($app.ExitCode)"};if([DateTime]::UtcNow -gt $deadline){throw 'Readiness deadline exceeded'};Start-Sleep -Milliseconds 100}
        $ready=Get-Content -LiteralPath ($prefix+'-ready.json') -Raw|ConvertFrom-Json
    }else{
        if(-not(Test-Path -LiteralPath 'C:/ZipshotLab/work/editor-stock22-app/.editor-owned-copy')){throw 'Run and visually verify the stock22 probe before interaction'}
        $dpi=[EditorGuestDrive]::GetDpiForSystem()
        $options=@{Thickness=4;BorderColorHex='#FFFF0000';Shadow=$false;BlurStrength=15;ZoomToFitOnOpen=$true;ShowExitConfirmation=$false;AutoCopyImageToClipboard=$false;AutoCloseEditorOnTask=$false;ShowNotifications=$false;RememberWindowState=$true;IsWindowMaximized=$false;WindowWidth=(1280.0*96/$dpi);WindowHeight=(900.0*96/$dpi);LastUsedAnnotationTool='Select';BackgroundMargin=0;BackgroundPadding=0;BackgroundSmartPadding=$false;BackgroundRoundedCorner=0;BackgroundShadowRadius=0}
        $config=@{ShowStartScreen=$false;ShowIconPackPrompt=$false;FirstTimeMinimizeToTray=$false;AutoCheckUpdate=$false;ShowTray=$true;ShowClipboardContentViewer=$false;DisableUpload=$true;Language='English';UseCustomScreenshotsPath=$true;CustomScreenshotsPath=$run;DefaultTaskSettings=@{AfterCaptureJob='None';AfterUploadJob='None';ToolsSettings=@{ImageEditorOptions=$options};GeneralSettings=@{PlaySoundAfterCapture=$false;PlaySoundAfterUpload=$false;PlaySoundAfterAction=$false}}}
        $config|ConvertTo-Json -Depth 12|Set-Content -LiteralPath 'C:/ZipshotLab/work/editor-stock22-app/ShareX/ApplicationConfig.json' -Encoding UTF8
        $workFixture=Join-Path $run 'working-fixture.png';Copy-Item -LiteralPath $Fixture -Destination $workFixture
        $sourceImage=New-Object Drawing.Bitmap($Fixture);$imageWidth=$sourceImage.Width;$imageHeight=$sourceImage.Height;$sourceImage.Dispose()
        $app=Start-Actor $exe @('-portable','-multi','-silent','-nohotkeys','-ImageEditor',$workFixture);$appStdout=$app.StandardOutput.ReadToEndAsync();$appStderr=$app.StandardError.ReadToEndAsync();$deadline=[DateTime]::UtcNow.AddSeconds(45);$stockWindow=$null
        while($null -eq $stockWindow){$stockWindow=@([EditorGuestDrive]::Windows([uint32]$app.Id)|Where-Object {($_.right-$_.left)-gt 600 -and ($_.bottom-$_.top)-gt 400 -and ($_.title -match 'working-fixture|Image.Editor|Image editor' -or $_.cls -match 'Avalonia')}|Select-Object -First 1)[0];if($app.HasExited){throw 'Stock22 exited before editor window'};if([DateTime]::UtcNow -gt $deadline){throw 'Stock22 window timeout'};Start-Sleep -Milliseconds 100}
        [EditorGuestDrive]::SetWindowPos([IntPtr]::new($stockWindow.hwnd),[IntPtr]::new(-1),40,40,0,0,0x41)|Out-Null;Start-Sleep -Milliseconds 600
        $stockWindow=@([EditorGuestDrive]::Windows([uint32]$app.Id)|Where-Object hwnd -eq $stockWindow.hwnd)[0]
        $ready=[pscustomobject]@{pid=$app.Id;hwnd=$stockWindow.hwnd.ToString('x');window=@($stockWindow.left,$stockWindow.top,$stockWindow.right,$stockWindow.bottom);image_size=@($imageWidth,$imageHeight);engine='Unmodified stock ShareX22 CLI';stock_editor_sha256=(Get-FileHash -LiteralPath 'C:/ZipshotLab/work/editor-stock22-app/ShareX.ImageEditor.dll').Hash}
    }
    if([int]$ready.pid -ne $app.Id){throw 'Owned process identity mismatch'}
    $report.ready=$ready;$hwnd=[IntPtr]::new([Convert]::ToInt64([string]$ready.hwnd,16));[uint32]$owner=0
    [EditorGuestDrive]::GetWindowThreadProcessId($hwnd,[ref]$owner)|Out-Null
    if($owner -ne $app.Id){throw 'UI HWND belongs to another process'}
    [EditorGuestDrive]::ShowWindow($hwnd,9)|Out-Null;[EditorGuestDrive]::BringWindowToTop($hwnd)|Out-Null;[EditorGuestDrive]::SetForegroundWindow($hwnd)|Out-Null
    Start-Sleep -Milliseconds 200
    [uint32]$foregroundPid=0;[EditorGuestDrive]::GetWindowThreadProcessId([EditorGuestDrive]::GetForegroundWindow(),[ref]$foregroundPid)|Out-Null
    if($foregroundPid -ne $app.Id){
        $titleX=[int]$ready.window[0]+120;$titleY=[int]$ready.window[1]+12
        $hit=[EditorGuestDrive]::WindowFromPoint([EditorGuestDrive+POINT]::new($titleX,$titleY));[uint32]$hitPid=0;[EditorGuestDrive]::GetWindowThreadProcessId($hit,[ref]$hitPid)|Out-Null
        if($hitPid -ne $app.Id){throw 'Owned editor title is occluded, refusing input'}
        Input-Move $titleX $titleY 'focus_setup';Input-Button $true 'focus_setup';Input-Button $false 'focus_setup';Start-Sleep -Milliseconds 200
    }
    [EditorGuestDrive]::GetWindowThreadProcessId([EditorGuestDrive]::GetForegroundWindow(),[ref]$foregroundPid)|Out-Null
    if($foregroundPid -ne $app.Id){throw 'Editor did not own focus, refusing drawing input'}
    $imageWidth=[int]$ready.image_size[0];$imageHeight=[int]$ready.image_size[1]
    $tool=if($Case -eq 'freehand'){70}elseif($Case -eq 'blur'){66}elseif($Case -eq 'text'){84}elseif($Case -eq 'pixelate'){80}elseif($Case -eq 'colorpicker'){82}else{65}
    if($Case -ne 'zoom'){Input-Key $tool $true 'tool_setup';Input-Key $tool $false 'tool_setup'}
    Start-Sleep -Milliseconds 350
    if($Engine -eq 'sharex22'){
        $viewport=Join-Path $run 'viewport-before.png';$frameW=([int]$ready.window[2]-[int]$ready.window[0]+1)-band(-bnot 1);$frameH=([int]$ready.window[3]-[int]$ready.window[1]+1)-band(-bnot 1)
        Capture-Still $viewport $ready.window[0] $ready.window[1] $frameW $frameH
        $sheet=[EditorGuestDrive]::FindSheet($viewport);$sx=($sheet.width-1)/(($imageWidth/1920.0)*1760);$sy=($sheet.height-1)/(($imageHeight/1080.0)*940)
        if($sheet.area -lt 10000 -or [Math]::Abs($sx-$sy) -gt .008 -or $sx -le 0){throw ('Cannot verify the complete fixture viewport, scales '+$sx+' / '+$sy)}
        $originX=$ready.window[0]+$sheet.x-($imageWidth/1920.0)*80*$sx;$originY=$ready.window[1]+$sheet.y-($imageHeight/1080.0)*70*$sx
        $ready|Add-Member -NotePropertyName source_origin -NotePropertyValue ([pscustomobject]@{x=$originX;y=$originY});$ready|Add-Member -NotePropertyName source_x1000 -NotePropertyValue ([pscustomobject]@{x=$originX+1000*$sx;y=$originY});$ready|Add-Member -NotePropertyName source_y1000 -NotePropertyValue ([pscustomobject]@{x=$originX;y=$originY+1000*$sx})
        $ready|Add-Member -NotePropertyName mapping_method -NotePropertyValue 'Pixel-derived whole white-sheet bounds from actual displayed stock editor, verified aspect ratio'
        $ready|Add-Member -NotePropertyName observed_scale_x -NotePropertyValue $sx;$ready|Add-Member -NotePropertyName observed_scale_y -NotePropertyValue $sy;$ready|Add-Member -NotePropertyName sheet_bounds -NotePropertyValue $sheet
        $ready|ConvertTo-Json -Depth 12|Set-Content -LiteralPath ($prefix+'-ready.json') -Encoding UTF8;$report.ready=$ready
    }
    $left=[int]$ready.window[0];$top=[int]$ready.window[1];$captureWidth=([int]$ready.window[2]-$left+1)-band(-bnot 1);$captureHeight=([int]$ready.window[3]-$top+1)-band(-bnot 1)
    if($Case -eq 'colorpicker'){$controlShot=Join-Path $run 'color-control-before.png';Capture-Still $controlShot $left $top $captureWidth $captureHeight;$observedColorControl=[EditorGuestDrive]::RedControl($controlShot);$captureWidth+=420}
    $clip=Join-Path $run 'actual-ui.mkv';$progress=Join-Path $run 'capture-progress.txt'
    $captureArguments=@('-hide_banner','-loglevel','info','-y','-copyts','-f','gdigrab','-framerate','60','-draw_mouse','0','-offset_x',$left,'-offset_y',$top,'-video_size',($captureWidth.ToString()+'x'+$captureHeight),'-use_wallclock_as_timestamps','1','-i','desktop','-an','-c:v','libx264rgb','-preset','ultrafast','-crf','0','-threads','2','-fps_mode','passthrough','-avoid_negative_ts','disabled','-flush_packets','1','-progress',$progress,'-stats_period','0.2',$clip)
    $report.clock_before_capture=Clock-Anchor
    $capture=Start-Actor 'C:/ZipshotLab/input/ffmpeg.exe' $captureArguments $true;$captureStderr=$capture.StandardError.ReadToEndAsync();$captureStdout=$capture.StandardOutput.ReadToEndAsync()
    $captureDeadline=[DateTime]::UtcNow.AddSeconds(12)
    do{if($capture.HasExited){throw 'Capture process exited early'};Start-Sleep -Milliseconds 100;$started=(Test-Path -LiteralPath $progress)-and (Get-Content -LiteralPath $progress -Raw)-match 'frame=[1-9][0-9]*'}while(-not $started -and [DateTime]::UtcNow -lt $captureDeadline)
    if(-not $started){throw 'No actual captured frame before deadline'}
    Start-Sleep -Milliseconds 300;$report.clock_before_input=Clock-Anchor
    $driverTimerResult=[EditorGuestDrive]::timeBeginPeriod(1);$report.driver_timer_resolution_ms=if($driverTimerResult -eq 0){1}else{$null}
    if($Case -in @('arrow','freehand','pixelate')){Draw-Stroke $Case $Case}
    elseif($Case -eq 'text'){
        $point=Image-Point ($imageWidth*.43) ($imageHeight*.43);Input-Move $point.x $point.y 'text';Input-Button $true 'text';Input-Button $false 'text';Start-Sleep -Milliseconds 180;$start=[EditorGuestDrive]::Qpc()
        foreach($character in 'Check this value'.ToCharArray()){$before=[EditorGuestDrive]::Qpc();[EditorGuestDrive]::Character($character);$samples.Add(@{kind='text_character';phase='text';character=[string]$character;qpc_before=$before;qpc_after=[EditorGuestDrive]::Qpc()});Start-Sleep -Milliseconds 55}
        if($Engine -eq 'native'){Input-Key 27 $true 'text_commit';Input-Key 27 $false 'text_commit'}else{Input-Move ($left+148) ($top+82) 'text_commit';Input-Button $true 'text_commit';Input-Button $false 'text_commit'}
        $phases.Add(@{name='text';qpc_start=$start;qpc_end=[EditorGuestDrive]::Qpc();text='Check this value';scope='Untimed feature demonstration with each application font defaults'})
    }
    elseif($Case -eq 'colorpicker'){
        $start=[EditorGuestDrive]::Qpc();Input-Move ($left+$observedColorControl.x) ($top+$observedColorControl.y) 'colorpicker';Input-Button $true 'colorpicker';Input-Button $false 'colorpicker';Start-Sleep -Milliseconds 2000
        $phases.Add(@{name='colorpicker';qpc_start=$start;qpc_end=[EditorGuestDrive]::Qpc();scope='Untimed native color-picker popup, clicked actual observed red toolbar control'})
    }
    elseif($Case -eq 'blur'){Draw-Stroke 'blur' 'blur_cold';Start-Sleep -Milliseconds 700;Shortcut 90 'undo_for_repeat';Start-Sleep -Milliseconds 400;Draw-Stroke 'blur' 'blur_repeat'}
    elseif($Case -eq 'undo'){
        Draw-Stroke 'arrow' 'setup_arrow';Start-Sleep -Milliseconds 500;Input-Key 17 $true 'undo_redo';$start=[EditorGuestDrive]::Qpc()
        for($index=0;$index -lt 30;$index++){[EditorGuestDrive]::Wait($start+[long]($index*.1*$frequency));$code=if($index%2 -eq 0){90}else{89};Input-Key $code $true 'undo_redo';Input-Key $code $false 'undo_redo'}
        Input-Key 17 $false 'undo_redo';$phases.Add(@{name='undo_redo';qpc_start=$start;qpc_end=[EditorGuestDrive]::Qpc();actions=30;interval_ms=100})
    }else{
        $center=Image-Point ($imageWidth/2) ($imageHeight/2);Input-Move $center.x $center.y 'zoom_setup';Input-Key 17 $true 'zoom';$start=[EditorGuestDrive]::Qpc()
        for($index=0;$index -lt 30;$index++){[EditorGuestDrive]::Wait($start+[long]($index*.1*$frequency));$delta=if($index%2 -eq 0){120}else{-120};$before=[EditorGuestDrive]::Qpc();[EditorGuestDrive]::Wheel($delta);$samples.Add(@{kind='wheel';phase='zoom';index=$index;delta=$delta;qpc_before=$before;qpc_after=[EditorGuestDrive]::Qpc()})}
        Input-Key 17 $false 'zoom';$phases.Add(@{name='zoom';qpc_start=$start;qpc_end=[EditorGuestDrive]::Qpc();actions=30;interval_ms=100})
    }
    $report.clock_after_input=Clock-Anchor;Start-Sleep -Milliseconds 650
    $capture.StandardInput.WriteLine('q');if(-not $capture.WaitForExit(15000)){throw 'Capture did not stop after q'}
    $captureStderr.Result|Set-Content -LiteralPath (Join-Path $run 'capture.log')
    if($capture.ExitCode -ne 0){throw "Capture failed $($capture.ExitCode)"}
    if($Case -eq 'colorpicker'){Input-Key 27 $true 'close_colorpicker';Input-Key 27 $false 'close_colorpicker';Start-Sleep -Milliseconds 250}
    if($Engine -eq 'native'){Native-Message (0x8000+302)}else{Shortcut 83 'post_capture_export';Start-Sleep -Milliseconds 800;Copy-Item -LiteralPath $workFixture -Destination ($prefix+'-snapshot.png');@{method='Stock Ctrl+S attempted after captured input';source='Working fixture file, must be pixel-verified for actual saved edits';width=$imageWidth;height=$imageHeight}|ConvertTo-Json|Set-Content -LiteralPath ($prefix+'-snapshot.json') -Encoding UTF8}
    $snapshotDeadline=[DateTime]::UtcNow.AddSeconds(15)
    while(-not(Test-Path -LiteralPath ($prefix+'-snapshot.json'))){if([DateTime]::UtcNow -gt $snapshotDeadline){throw 'Final actual export missing'};Start-Sleep -Milliseconds 100}
    if($Engine -eq 'native'){Native-Message (0x8000+301)}else{@{engine='Stock ShareX22';scope='No internal model hook. Actual captured pixels and saved output require offline verification.'}|ConvertTo-Json|Set-Content -LiteralPath ($prefix+'-final.json') -Encoding UTF8;$app.CloseMainWindow()|Out-Null;Start-Sleep -Milliseconds 300;if(-not $app.HasExited){$app.Kill()}}
    if(-not $app.WaitForExit(10000)){throw 'Owned editor failed to stop'}
    $rawProbe=& 'C:/ZipshotLab/input/ffprobe.exe' -v error -show_streams -show_format -show_frames -show_entries 'frame=pts_time,best_effort_timestamp_time,pkt_duration_time,key_frame:stream=index,width,height,r_frame_rate,avg_frame_rate,time_base,start_time,duration,nb_frames:format=start_time,duration,size' -of json $clip
    if($LASTEXITCODE -ne 0){throw 'Actual clip decode/probe failed'}
    [IO.File]::WriteAllText((Join-Path $run 'video-frames.json'),($rawProbe-join "`n"))
    $video=$rawProbe-join "`n"|ConvertFrom-Json;$timestamps=@($video.frames|ForEach-Object {[double]$_.best_effort_timestamp_time})
    if($timestamps.Count -lt 3){throw 'Too few captured frames'}
    $report.video=[ordered]@{file='actual-ui.mkv';sha256=(Get-FileHash -LiteralPath $clip).Hash;bytes=(Get-Item -LiteralPath $clip).Length;capture_requested_fps=60;cursor_drawn_by_observer=$false;capture_crop=@($left,$top,$captureWidth,$captureHeight);frame_count=$timestamps.Count;first_pts=$timestamps[0];last_pts=$timestamps[-1];observed_pts_fps=($timestamps.Count-1)/($timestamps[-1]-$timestamps[0]);epoch_pts=($timestamps[0] -gt 1000000000);capture_codec='libx264rgb lossless, threads2, passthrough timestamps';timing_limit='Actual capture PTS and duplicate analysis govern timing claims, not requested FPS'}
    $report.final=Get-Content -LiteralPath ($prefix+'-final.json') -Raw|ConvertFrom-Json
    $report.snapshot=Get-Content -LiteralPath ($prefix+'-snapshot.json') -Raw|ConvertFrom-Json
    $report.status='captured_pending_visual_and_frame_analysis'
}catch{$report.status='fail';$report.error=$_.Exception.Message}
finally{
    if($driverTimerResult -eq 0){[EditorGuestDrive]::timeEndPeriod(1)|Out-Null}
    try{[EditorGuestDrive]::Button($false);[EditorGuestDrive]::Key(17,$false)}catch{}
    if($null -ne $capture -and -not $capture.HasExited){try{$capture.StandardInput.WriteLine('q');$capture.WaitForExit(5000)|Out-Null}catch{};if(-not $capture.HasExited){$capture.Kill();$capture.WaitForExit()}}
    if($null -ne $app -and -not $app.HasExited){if($Engine -eq 'sharex22'){[IO.File]::WriteAllText($prefix+'-command.txt','stop')};if(-not $app.WaitForExit(3000)){$app.Kill();$app.WaitForExit()}}
    if($null -ne $app){$appStdout.Result|Set-Content -LiteralPath (Join-Path $run 'app-stdout.txt');$appStderr.Result|Set-Content -LiteralPath (Join-Path $run 'app-stderr.txt')}
    if($null -ne $captureStderr){$captureStderr.Result|Set-Content -LiteralPath (Join-Path $run 'capture.log')}
    $report.qpc_frequency=$frequency;$report.injected_events=$samples.ToArray();$report.phases=$phases.ToArray();$report.utc_end=[DateTime]::UtcNow.ToString('o')
    $path=Join-Path $run 'editor-interaction-result.json';$report|ConvertTo-Json -Depth 17|Set-Content -LiteralPath $path -Encoding UTF8;Write-Output $path
}

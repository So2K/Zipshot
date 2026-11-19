# Real input and real-time window recording, strictly inside the approved disposable guest.
param([ValidateSet('native','sharex22')][string]$Engine='native',[ValidateSet('arrow','freehand')][string]$Case='arrow',[string]$Fixture='C:/ZipshotLab/input/fixtures/editor-fixture-1920x1080.png',[string]$OutputRoot='C:/ZipshotLab/output/editor-region',[string]$NativeExe='C:/ZipshotLab/input/editor_ui_native_r3.exe',[string]$ShareXExe='C:/ZipshotLab/input/editor22_r3/ShareX.exe')
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount' -or [Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0 -or $env:ZIPSHOT_RUNTIME_ISOLATED -ne '1' -or -not(Test-Path -LiteralPath 'C:/ZipshotLab/input/.isolation-approved.json')){throw 'Refusing input or capture outside the approved isolated guest'}
$run=Join-Path $OutputRoot ($Engine+'-'+$Case+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $run -Force|Out-Null
$prefix=Join-Path $run 'editor'
$report=[ordered]@{schema_version=1;status='running';engine=$Engine;case=$Case;utc_start=[DateTime]::UtcNow.ToString('o');source_background='Actual disposable guest desktop. Fixture parameter is accepted by the common wrapper but no supplied image is loaded.';scope='Full unmodified application, Ctrl+PrintScreen then A or F. Actual region-overlay drawing, application defaults. Workflow evidence, not standalone editor timing.';output_prefix=$prefix}
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
 [DllImport("user32.dll")]public static extern int GetSystemMetrics(int index);
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
$driverTimerResult=[EditorGuestDrive]::timeBeginPeriod(1)
try{
    $exe=if($Engine -eq 'native'){'C:/ZipshotLab/work/editor-region-zipshot/zipshot.exe'}else{'C:/ZipshotLab/work/editor-stock22-app/ShareX.exe'}
    $save=Join-Path $run 'saved';New-Item -ItemType Directory -Path $save -Force|Out-Null
    if($Engine -eq 'native'){
        $local=Split-Path $exe -Parent;New-Item -ItemType Directory -Path $local -Force|Out-Null
        Copy-Item -LiteralPath 'C:/ZipshotLab/input/zipshot/zipshot.exe' -Destination $exe
        @"
[main]
Language=en
ShareXImported=1
SaveDir=$save
AfterCopy=1
AfterSave=1
AfterToast=0
AfterEditor=0
ShowCursor=0
Sound=0
SoundCapture=0
SoundDone=0
SoundAction=0
JpegOverKB=0
[hotkeys]
Full=PrintScreen
Region=Ctrl+PrintScreen
Window=Alt+PrintScreen
"@|Set-Content -LiteralPath (Join-Path $local 'zipshot.ini') -Encoding Unicode
        $arguments=@()
    }else{
        if(-not(Test-Path -LiteralPath 'C:/ZipshotLab/work/editor-stock22-app/.editor-owned-copy')){throw 'Verified stock22 package missing'}
        $config=@{ShowStartScreen=$false;ShowIconPackPrompt=$false;FirstTimeMinimizeToTray=$false;AutoCheckUpdate=$false;ShowTray=$true;ShowClipboardContentViewer=$false;DisableUpload=$true;DisableHotkeys=$false;UseCustomScreenshotsPath=$true;CustomScreenshotsPath=$save;SaveImageSubFolderPattern='';Language='English';DefaultTaskSettings=@{AfterCaptureJob='CopyImageToClipboard, SaveImageToFile';AfterUploadJob='None';ImageSettings=@{ImageFormat='PNG';ImageAutoUseJPEG=$false};CaptureSettings=@{ShowCursor=$false};GeneralSettings=@{PlaySoundAfterCapture=$false;PlaySoundAfterUpload=$false;PlaySoundAfterAction=$false;ShowToastNotificationAfterTaskCompleted=$false}}}
        $config|ConvertTo-Json -Depth 12|Set-Content -LiteralPath 'C:/ZipshotLab/work/editor-stock22-app/ShareX/ApplicationConfig.json' -Encoding UTF8
        $arguments=@('-portable','-multi','-silent')
    }
    $app=Start-Actor $exe $arguments;$appStdout=$app.StandardOutput.ReadToEndAsync();$appStderr=$app.StandardError.ReadToEndAsync()
    Start-Sleep -Milliseconds 4000
    if($app.HasExited){throw 'Stock app exited before region hotkey'}
    $width=[EditorGuestDrive]::GetSystemMetrics(0);$height=[EditorGuestDrive]::GetSystemMetrics(1);if($width-ne2982-or$height-ne1672){throw 'Unexpected guest resolution; trajectory must remain unchanged'}
    $beforeWindows=@([EditorGuestDrive]::Windows([uint32]$app.Id))
    $report.application=@{exe=$exe;pid=$app.Id;sha256=(Get-FileHash -LiteralPath $exe).Hash;version=(Get-Item -LiteralPath $exe).VersionInfo.ProductVersion;unmodified_exe=$true;fresh_task_owned_profile=$true;display=@($width,$height)}
    $clip=Join-Path $run 'actual-ui.mkv'
    $capture=Start-Actor 'C:/ZipshotLab/input/ffmpeg.exe' @('-hide_banner','-loglevel','info','-y','-probesize','32','-analyzeduration','0','-use_wallclock_as_timestamps','1','-f','gdigrab','-framerate','30','-draw_mouse','1','-offset_x','0','-offset_y','0','-video_size',($width.ToString()+'x'+$height),'-i','desktop','-copyts','-an','-c:v','libx264rgb','-preset','ultrafast','-crf','0','-threads','2','-fps_mode','passthrough',$clip) $true
    $captureStderr=$capture.StandardError.ReadToEndAsync();$captureStdout=$capture.StandardOutput.ReadToEndAsync()
    Start-Sleep -Milliseconds 900
    if($capture.HasExited){throw 'Real desktop recorder failed'}
    $report.clock_before_input=Clock-Anchor
    Shortcut 44 'region_hotkey'
    $deadline=[DateTime]::UtcNow.AddSeconds(12);$overlay=$null
    while($null -eq $overlay){
        $windows=@([EditorGuestDrive]::Windows([uint32]$app.Id))
        $overlay=@($windows|Where-Object {($_.right-$_.left)-gt $width*.8 -and ($_.bottom-$_.top)-gt $height*.8 -and $_.hwnd -notin @($beforeWindows.hwnd)}|Select-Object -First 1)[0]
        if([DateTime]::UtcNow -gt $deadline){$report.windows_after_hotkey=$windows;throw 'No new actual owned full-screen region overlay'}
        Start-Sleep -Milliseconds 50
    }
    $report.overlay=$overlay
    [uint32]$foregroundPid=0;[EditorGuestDrive]::GetWindowThreadProcessId([EditorGuestDrive]::GetForegroundWindow(),[ref]$foregroundPid)|Out-Null
    if($foregroundPid -ne $app.Id){throw 'Actual region overlay is not foreground; refusing drawing'}
    Start-Sleep -Milliseconds 400
    Capture-Still (Join-Path $run 'overlay-before.png') 0 0 $width $height
    $key=if($Case -eq 'arrow'){65}else{70}
    Input-Key $key $true 'select_tool';Input-Key $key $false 'select_tool'
    Start-Sleep -Milliseconds 250
    $points=New-Object 'Collections.Generic.List[object]'
    for($i=0;$i-lt120;$i++){
        $t=$i/119.0;$x=[int][Math]::Round(900+1000*$t)
        $y=if($Case -eq 'arrow'){[int][Math]::Round(1000-400*$t)}else{[int][Math]::Round(800+170*[Math]::Sin(6*[Math]::PI*$t))}
        $points.Add(@{x=$x;y=$y})
    }
    Input-Move $points[0].x $points[0].y $Case 0
    $start=[EditorGuestDrive]::Qpc();Input-Button $true $Case
    for($i=1;$i-lt120;$i++){
        $scheduled=$start+[long]($frequency*2.0*$i/119)
        [EditorGuestDrive]::Wait($scheduled);Input-Move $points[$i].x $points[$i].y $Case $i $scheduled
    }
    Input-Button $false $Case
    $phases.Add(@{name=$Case;kind=$Case;qpc_start=$start;qpc_end=[EditorGuestDrive]::Qpc();point_count=120;nominal_duration_ms=2000;points=$points.ToArray();scope='Actual region overlay, screen-coordinate trajectory, application default annotation style'})
    $report.clock_after_input=Clock-Anchor
    Start-Sleep -Milliseconds 1000
    Capture-Still (Join-Path $run 'overlay-after.png') 0 0 $width $height
    Start-Sleep -Milliseconds 350
    $capture.StandardInput.WriteLine('q')
    if(-not $capture.WaitForExit(20000)){throw 'Recorder stop timeout'}
    if($capture.ExitCode-ne0){throw ('Recorder failed '+$captureStderr.Result)}
    Input-Key 32 $true 'save_fullscreen';Input-Key 32 $false 'save_fullscreen'
    Start-Sleep -Milliseconds 1600
    $saved=@(Get-ChildItem -LiteralPath $save -Recurse -File|Where-Object Extension -in '.png','.jpg','.jpeg'|ForEach-Object {@{file=$_.FullName;bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}})
    $report.saved_files=$saved
    $rawProbe=& 'C:/ZipshotLab/input/ffprobe.exe' -v error -show_streams -show_frames -show_entries 'frame=pts_time,best_effort_timestamp_time:stream=width,height,r_frame_rate,avg_frame_rate,time_base' -of json $clip
    if($LASTEXITCODE-ne0){throw 'Actual clip probe failed'}
    [IO.File]::WriteAllText((Join-Path $run 'video-frames.json'),($rawProbe-join [Environment]::NewLine))
    $video=$rawProbe-join [Environment]::NewLine|ConvertFrom-Json;$timestamps=@($video.frames|ForEach-Object {[double]$_.best_effort_timestamp_time})
    if($timestamps.Count-lt3){throw 'Too few genuine recorded frames'}
    $report.video=@{file='actual-ui.mkv';sha256=(Get-FileHash -LiteralPath $clip).Hash;bytes=(Get-Item -LiteralPath $clip).Length;capture_requested_fps=30;cursor_drawn_by_observer=$true;capture_crop=@(0,0,$width,$height);frame_count=$timestamps.Count;first_pts=$timestamps[0];last_pts=$timestamps[-1];observed_pts_fps=($timestamps.Count-1)/($timestamps[-1]-$timestamps[0]);epoch_pts=($timestamps[0]-gt1000000000);capture_codec='libx264rgb lossless, threads2, passthrough PTS';time_policy='Actual real-time 1x, normalize epoch origin only'}
    $report.style='Fresh application defaults. Color, width, smoothing and shadow may differ. This is workflow evidence, not a quality-matched speed comparison.'
    $report.status='captured_pending_visual_and_frame_analysis'
}catch{$report.status='fail';$report.error=$_.Exception.Message}
finally{
    if($driverTimerResult-eq0){[EditorGuestDrive]::timeEndPeriod(1)|Out-Null}
    try{[EditorGuestDrive]::Button($false);[EditorGuestDrive]::Key(17,$false);[uint32]$cleanupPid=0;[EditorGuestDrive]::GetWindowThreadProcessId([EditorGuestDrive]::GetForegroundWindow(),[ref]$cleanupPid)|Out-Null;if($null-ne$app-and$cleanupPid-eq$app.Id){[EditorGuestDrive]::Key(27,$true);[EditorGuestDrive]::Key(27,$false)}}catch{}
    if($null-ne$capture-and-not$capture.HasExited){try{$capture.StandardInput.WriteLine('q');$capture.WaitForExit(5000)|Out-Null}catch{};if(-not$capture.HasExited){$capture.Kill();$capture.WaitForExit()}}
    if($null-ne$app-and-not$app.HasExited){$app.CloseMainWindow()|Out-Null;if(-not$app.WaitForExit(2000)){$app.Kill();$app.WaitForExit()}}
    if($null-ne$appStdout){$appStdout.Result|Set-Content -LiteralPath (Join-Path $run 'app-stdout.txt');$appStderr.Result|Set-Content -LiteralPath (Join-Path $run 'app-stderr.txt')}
    if($null-ne$captureStderr){$captureStderr.Result|Set-Content -LiteralPath (Join-Path $run 'capture.log')}
    $report.qpc_frequency=$frequency;$report.injected_events=$samples.ToArray();$report.phases=$phases.ToArray();$report.utc_end=[DateTime]::UtcNow.ToString('o')
    $path=Join-Path $run 'editor-region-result.json';$report|ConvertTo-Json -Depth 17|Set-Content -LiteralPath $path -Encoding UTF8;Write-Output $path
}

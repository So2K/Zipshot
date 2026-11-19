param(
    [string]$InputRoot='C:/ZipshotLab/input/recording',
    [string]$OutputRoot='C:/ZipshotLab/output/recording',
    [string]$WorkRoot='C:/ZipshotLab/work/recording',
    [int]$Seconds=30,
    [switch]$FixtureOnly,
    [string]$Cases='zipshot-video30,sharex-video30,zipshot-video60,sharex-video60,zipshot-gif15,sharex-gif15',
    [string]$ShareXBackend='gdigrab',
    [ValidateSet('auto','x264','nvenc')][string]$VideoProfile='auto',
    [ValidateSet('mapped','local')][string]$ExecutableLocation='local',
    [ValidateSet('factory','ultrafast','superfast','veryfast','faster','fast','medium','slow','slower','veryslow','placebo')][string]$SoftwarePreset='veryfast'
)
$ErrorActionPreference='Stop'
if($env:USERNAME -ne 'WDAGUtilityAccount'){throw 'Recording is restricted to the clean Windows Sandbox guest.'}
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
if(-not $OutputRoot.StartsWith('C:\ZipshotLab\output\',[StringComparison]::OrdinalIgnoreCase)){throw 'Output must be the task-owned sandbox mapping.'}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$WorkRoot=[IO.Path]::GetFullPath($WorkRoot)
if(-not $WorkRoot.StartsWith('C:\ZipshotLab\work\',[StringComparison]::OrdinalIgnoreCase)){throw 'WorkRoot must be inside the clean guest task work folder.'}
if(-not $FixtureOnly){$WorkRoot=Join-Path $WorkRoot ('run-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'));New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null}
if($SoftwarePreset -ne 'veryfast' -and $Cases -match '(^|,)sharex-'){throw 'SoftwarePreset controls the Zipshot INI only. Use Zipshot-only cases for this preset probe; the matched legacy ShareX harness remains veryfast.'}
if($Cases -eq 'diagnostic-image-locality'){
 & (Join-Path $InputRoot 'recording_image_io_guest.ps1') -Source (Join-Path $InputRoot 'ffmpeg.exe') -OutputRoot $OutputRoot
 exit
}
if($Cases -eq 'diagnostic-encoder-threads'){
 & (Join-Path $InputRoot 'recording_encoder_threads_guest.ps1') -InputRoot $InputRoot -OutputRoot $OutputRoot
 exit
}

if($FixtureOnly){
    $nativeFixture=Join-Path $InputRoot 'recording_fixture.exe'
    if(Test-Path -LiteralPath $nativeFixture){
        $fixtureStart=[Diagnostics.ProcessStartInfo]::new($nativeFixture,('"'+$OutputRoot+'"'))
        $fixtureStart.UseShellExecute=$false;$fixtureStart.CreateNoWindow=$true
        $native=[Diagnostics.Process]::Start($fixtureStart);$native.WaitForExit();exit $native.ExitCode
    }
    Add-Type -AssemblyName System.Windows.Forms,System.Drawing
    Add-Type -ReferencedAssemblies System.Windows.Forms,System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Diagnostics;
using System.IO;
using System.Windows.Forms;
public class RecordingFixture:Form {
    readonly Stopwatch clock=Stopwatch.StartNew(); readonly Timer timer=new Timer();
    readonly Bitmap backdrop=new Bitmap(1920,1080); readonly string reset; DateTime last;
    public RecordingFixture(string folder){
        reset=Path.Combine(folder,"fixture-reset.txt");
        FormBorderStyle=FormBorderStyle.None; StartPosition=FormStartPosition.Manual; Bounds=new Rectangle(0,0,1920,1080); TopMost=true;
        DoubleBuffered=true; ShowInTaskbar=false;
        using(Graphics g=Graphics.FromImage(backdrop)){
            using(var b=new LinearGradientBrush(new Rectangle(0,0,1920,1080),Color.FromArgb(19,45,81),Color.FromArgb(191,115,49),35))g.FillRectangle(b,0,0,1920,1080);
            using(var b=new SolidBrush(Color.FromArgb(35,255,255,255)))for(int y=0;y<1080;y+=40)for(int x=0;x<1920;x+=40)if((x/40+y/40)%2==0)g.FillRectangle(b,x,y,40,40);
        }
        timer.Interval=15;timer.Tick+=(s,e)=>{if(File.Exists(reset)){DateTime stamp=File.GetLastWriteTimeUtc(reset);if(stamp!=last){last=stamp;clock.Restart();}}Invalidate();};timer.Start();
    }
    protected override void OnPaint(PaintEventArgs e){
        Graphics g=e.Graphics;g.DrawImageUnscaled(backdrop,0,0);
        int frame=(int)(clock.Elapsed.TotalSeconds*30),x=180+(frame*13)%1400,y=300+(frame*7)%420;
        using(var b=new SolidBrush(Color.FromArgb(238,228,101)))g.FillRectangle(b,x,y,180,180);
        using(var b=new SolidBrush(Color.FromArgb(19,22,30)))for(int i=0;i<6;i++)for(int j=0;j<6;j++)if((i+j)%2==0)g.FillRectangle(b,x+i*30,y+j*30,30,30);
        for(int i=0;i<16;i++)g.FillRectangle((frame&(1<<i))!=0?Brushes.White:Brushes.Black,32+i*20,32,16,16);
        using(var font=new Font("Segoe UI",32))g.DrawString("Zipshot lab  |  frame "+frame,font,Brushes.White,32,85);
        using(var font=new Font("Segoe UI",18))g.DrawString("Original synthetic fixture. No personal screen content.",font,Brushes.White,32,1000);
    }
}
'@
    $fixture=[RecordingFixture]::new($OutputRoot)
    [System.Windows.Forms.Application]::Run($fixture)
    exit
}

# Retain handles through process exit so final CPU accounting includes short-lived FFmpeg variants.
Add-Type -TypeDefinition @'
using System;using System.Collections.Generic;using System.Runtime.InteropServices;
public class RecordingProcessMetrics {
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Auto)]struct Entry{public uint size,usage,pid;public IntPtr heap;public uint module,threads,parent;public int priority;public uint flags;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=260)]public string name;}
 [StructLayout(LayoutKind.Sequential)]struct Mem{public uint cb,pageFaults;public UIntPtr peakWS,ws,peakPaged,paged,peakNonPaged,nonPaged,pagefile,peakPagefile,priv;}
 [DllImport("kernel32.dll")]static extern IntPtr CreateToolhelp32Snapshot(uint flags,uint pid);
 [DllImport("kernel32.dll",CharSet=CharSet.Auto)]static extern bool Process32First(IntPtr h,ref Entry e);
 [DllImport("kernel32.dll",CharSet=CharSet.Auto)]static extern bool Process32Next(IntPtr h,ref Entry e);
 [DllImport("kernel32.dll")]static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
 [DllImport("kernel32.dll")]static extern bool GetProcessTimes(IntPtr h,out long create,out long exit,out long kernel,out long user);
 [DllImport("kernel32.dll")]static extern bool GetExitCodeProcess(IntPtr h,out uint code);
 [DllImport("psapi.dll")]static extern bool GetProcessMemoryInfo(IntPtr h,out Mem m,uint cb);
 [DllImport("kernel32.dll")]static extern bool CloseHandle(IntPtr h);
 public class Row{public int pid,parent;public string name;public double cpu_seconds,cpu_user_seconds,cpu_kernel_seconds;public ulong private_bytes,working_set_bytes,peak_working_set_bytes;public bool alive;public uint exit_code;internal IntPtr handle;}
 readonly Dictionary<uint,Row> rows=new Dictionary<uint,Row>();
 public RecordingProcessMetrics(int pid){Add((uint)pid,0,"parent");}
 void Add(uint pid,uint parent,string name){IntPtr h=OpenProcess(0x410,false,pid);if(h==IntPtr.Zero)h=OpenProcess(0x1000,false,pid);if(h!=IntPtr.Zero)rows[pid]=new Row{pid=(int)pid,parent=(int)parent,name=name,handle=h};}
 public Row[] Sample(){
  var entries=new List<Entry>();IntPtr snap=CreateToolhelp32Snapshot(2,0);Entry e=new Entry();e.size=(uint)Marshal.SizeOf(typeof(Entry));
  if(Process32First(snap,ref e))do{entries.Add(e);}while(Process32Next(snap,ref e));CloseHandle(snap);
  for(int pass=0;pass<4;pass++)foreach(Entry item in entries)if(!rows.ContainsKey(item.pid)&&rows.ContainsKey(item.parent))Add(item.pid,item.parent,item.name);
  foreach(Row r in rows.Values){long c,x,k,u;uint code;
   if(GetProcessTimes(r.handle,out c,out x,out k,out u)){r.cpu_seconds=(k+u)/10000000.0;r.cpu_kernel_seconds=k/10000000.0;r.cpu_user_seconds=u/10000000.0;}
   if(GetExitCodeProcess(r.handle,out code)){r.exit_code=code;r.alive=code==259;}
   Mem m;if(GetProcessMemoryInfo(r.handle,out m,(uint)Marshal.SizeOf(typeof(Mem)))){r.private_bytes=m.priv.ToUInt64();r.working_set_bytes=m.ws.ToUInt64();r.peak_working_set_bytes=m.peakWS.ToUInt64();}
  }
  var copy=new List<Row>();foreach(Row r in rows.Values)copy.Add(new Row{pid=r.pid,parent=r.parent,name=r.name,cpu_seconds=r.cpu_seconds,cpu_user_seconds=r.cpu_user_seconds,cpu_kernel_seconds=r.cpu_kernel_seconds,private_bytes=r.private_bytes,working_set_bytes=r.working_set_bytes,peak_working_set_bytes=r.peak_working_set_bytes,alive=r.alive,exit_code=r.exit_code});return copy.ToArray();
 }
 public void Close(){foreach(Row r in rows.Values)CloseHandle(r.handle);}
}
'@

$ffmpeg=Join-Path $InputRoot 'ffmpeg.exe';$ffprobe=Join-Path $InputRoot 'ffprobe.exe'
if($ExecutableLocation -eq 'local'){
 $localInfo=& (Join-Path $InputRoot 'recording_local_prepare.ps1') -InputRoot (Split-Path $InputRoot -Parent)
 $ffmpeg=$localInfo.ffmpeg
 $localInfo | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputRoot 'recording_local_setup.json') -Encoding utf8
}
$probeInfo=[Diagnostics.ProcessStartInfo]::new($ffmpeg,'-hide_banner -loglevel warning -f lavfi -i color=size=128x128:rate=30 -frames:v 2 -c:v h264_nvenc -f null NUL')
$probeInfo.UseShellExecute=$false;$probeInfo.CreateNoWindow=$true;$probeInfo.RedirectStandardError=$true;$probeInfo.RedirectStandardOutput=$true
$probe=[Diagnostics.Process]::Start($probeInfo)
if(-not $probe.WaitForExit(15000)){$probe.Kill();$probe.WaitForExit()}
$nvencLog=$probe.StandardError.ReadToEnd()+$probe.StandardOutput.ReadToEnd();$nvencExit=$probe.ExitCode
$nvencLog | Set-Content -LiteralPath (Join-Path $OutputRoot 'recording_nvenc_probe.log') -Encoding utf8
@{schema_version=1;scope='Synthetic 2-frame encoder availability probe, not a product recording benchmark';nvenc_exit=$nvencExit;ffmpeg_sha256=(Get-FileHash -LiteralPath $ffmpeg -Algorithm SHA256).Hash.ToLowerInvariant()} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputRoot 'recording_encoder_probe.json') -Encoding utf8
if($VideoProfile -eq 'auto'){$VideoProfile=if($nvencExit -eq 0){'nvenc'}else{'x264'}}
if($nvencExit -eq 0 -and $VideoProfile -eq 'x264' -and ($Cases -match 'zipshot-video')){throw 'NVENC is available. Zipshot will prefer NVENC, so a matched x264 profile is not selectable in this release. Use auto/nvenc or GIF only.'}
$fixtureStart=[Diagnostics.ProcessStartInfo]::new((Join-Path $InputRoot 'recording_fixture.exe'),('"'+$WorkRoot+'"'))
$fixtureStart.UseShellExecute=$false;$fixtureStart.CreateNoWindow=$true
$fixture=[Diagnostics.Process]::Start($fixtureStart)
Start-Sleep -Seconds 2
$caseNames=$Cases.Split(',')
$report=[ordered]@{schema_version=1;scope='Clean Windows Sandbox only. Real product recording. Parent and FFmpeg children separately accounted.';io_note='Active recording, fixture control, process samples and GPU samples use the guest local work drive. Completed cases are copied to the task-owned host mapping after sampling closes.';executable_location=$ExecutableLocation;software_preset=$SoftwarePreset;guest_user=$env:USERNAME;logical_processors=[Environment]::ProcessorCount;sample_interval_ms=200;gpu='Per-engine PDH attempted. Filter raw samples to owned process IDs; unavailable counters are recorded explicitly.';video_profile=$VideoProfile;cases=@()}
$process=$null;$metrics=$null;$gpuJob=$null
try {
 foreach($caseName in $caseNames){
    $app,$kind=$caseName.Split('-');$gif=$kind.StartsWith('gif');$fps=if($gif){15}elseif($kind.EndsWith('60')){60}else{30}
    $caseRoot=Join-Path $WorkRoot $caseName;New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null
    if(Test-Path -LiteralPath (Join-Path $OutputRoot $caseName)){throw 'Published case already exists. Use a fresh OutputRoot for each run.'}
    Set-Content -LiteralPath (Join-Path $WorkRoot 'fixture-reset.txt') -Value $caseName
    if($app -eq 'zipshot'){
       $stage=Join-Path $caseRoot 'app';New-Item -ItemType Directory -Path $stage -Force | Out-Null
       Copy-Item -LiteralPath (Join-Path $InputRoot 'zipshot.exe') -Destination $stage
       $presetLine=if($SoftwarePreset -eq 'factory'){''}else{'X264Preset='+$SoftwarePreset}
       $ini=@"
[main]
SaveDir=$caseRoot
SubFolderPattern=
NamePattern=record
NamePatternWindow=record
FFmpeg=$ffmpeg
Sound=0
ShareXImported=1
AfterCopy=0
AfterToast=0
[record]
VideoFps=$fps
GifFps=15
ShowCursor=0
$presetLine
X264Crf=23
AudioSource=
AutoStart=1
StartDelay=0
GifStatsMode=full
GifDither=sierra2_4a
"@
       Set-Content -LiteralPath (Join-Path $stage 'zipshot.ini') -Value $ini -Encoding Unicode
       $exe=Join-Path $stage 'zipshot.exe';$mode=if($gif){'gif'}else{'video'}
       $arguments="--test-record $mode $Seconds 0 0 1920 1080"
    } else {
       $stage=Join-Path $WorkRoot 'sharex-runtime'
       if(-not(Test-Path -LiteralPath $stage)){Copy-Item -LiteralPath (Join-Path $InputRoot 'sharex-runtime') -Destination $stage -Recurse}
       $exe=Join-Path $stage 'ShareX.exe';$mode=if($gif){'gif'}else{'video'}
       $profile=if($VideoProfile -eq 'nvenc'){'matched-nvenc'}else{'matched'}
       $arguments='full "'+$caseRoot+'" "'+$ffmpeg+'" '+$fps+' '+$Seconds+' '+$ShareXBackend+' '+$mode+' '+$profile
    }
    $start=[Diagnostics.ProcessStartInfo]::new($exe,$arguments);$start.UseShellExecute=$false;$start.WorkingDirectory=$stage;$start.CreateNoWindow=$true
    $gpuPath=Join-Path $caseRoot 'recording_gpu_samples.jsonl'
    $gpuJob=Start-Job -ArgumentList $gpuPath,($Seconds+110) -ScriptBlock {
       param($path,$max)
       try {
          Get-Counter -Counter '\GPU Engine(*)\Utilization Percentage' -SampleInterval 1 -MaxSamples $max -ErrorAction Stop | ForEach-Object {
             $batch=@($_.CounterSamples | ForEach-Object {@{instance=$_.InstanceName;percent=$_.CookedValue}})
             @{utc=$_.Timestamp.ToUniversalTime().ToString('o');engines=$batch} | ConvertTo-Json -Depth 4 -Compress | Add-Content -LiteralPath $path -Encoding utf8
          }
       } catch {@{unavailable=$_.Exception.GetType().Name} | ConvertTo-Json -Compress | Set-Content -LiteralPath $path -Encoding utf8}
    }
    $startUtc=[DateTime]::UtcNow
    $clock=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($start)
    $metrics=[RecordingProcessMetrics]::new($process.Id);$samples=[Collections.Generic.List[object]]::new()
    $seenChildren=@{};$childCommands=@()
    $limit=$Seconds+120;$timedOut=$false
    while($true){
       $rows=@($metrics.Sample())
       $samples.Add([ordered]@{elapsed_seconds=$clock.Elapsed.TotalSeconds;processes=$rows})
       foreach($row in $rows){
          if($row.name -eq 'ffmpeg.exe' -and -not $seenChildren.ContainsKey($row.pid)){
             $seenChildren[$row.pid]=$true
             # Only exact FFmpeg descendants of the owned guest test process. Never enumerate host command lines.
             $child=Get-CimInstance Win32_Process -Filter ('ProcessId='+$row.pid) -ErrorAction SilentlyContinue
             $childCommands+=@{pid=$row.pid;first_seen_seconds=$clock.Elapsed.TotalSeconds;command=$child.CommandLine}
          }
       }
       if($process.HasExited -and -not($rows | Where-Object alive)){break}
       if($clock.Elapsed.TotalSeconds -gt $limit){$timedOut=$true;break}
       Start-Sleep -Milliseconds 200
    }
    if($timedOut){foreach($row in $rows | Where-Object alive){Stop-Process -Id $row.pid -Force -ErrorAction SilentlyContinue}}
    $processWall=$clock.Elapsed.TotalSeconds
    $final=@($metrics.Sample());$metrics.Close();$metrics=$null;$process.Refresh()
    Stop-Job -Job $gpuJob -ErrorAction SilentlyContinue;Remove-Job -Job $gpuJob -Force -ErrorAction SilentlyContinue
    $childCommands | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $caseRoot 'recording_ffmpeg_commands.json') -Encoding utf8
    $samplePath=Join-Path $caseRoot 'recording_samples.json';$samples | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $samplePath -Encoding utf8
    $files=@(Get-ChildItem -LiteralPath $caseRoot -Recurse -File | Where-Object {$_.Extension -in @('.mp4','.gif')})
    $probes=@()
    foreach($file in $files){
       $priorErrorAction=$ErrorActionPreference
       try{$ErrorActionPreference='Continue';$raw=& $ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=codec_name,width,height,avg_frame_rate,r_frame_rate,nb_read_frames,duration,pix_fmt -show_entries format=duration,size -of json $file.FullName 2>&1;$probeCode=$LASTEXITCODE}finally{$ErrorActionPreference=$priorErrorAction}
       $parsed=try{($raw -join "`n") | ConvertFrom-Json}catch{$null}
       $probes+=@{relative_path=$file.FullName.Substring($WorkRoot.Length+1);bytes=$file.Length;ffprobe_exit=$probeCode;ffprobe=$parsed;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
    if($app -eq 'zipshot'){
       foreach($name in @('zipshot.log','zipshot_ffmpeg.log')){if(Test-Path -LiteralPath (Join-Path $env:TEMP $name)){Copy-Item -LiteralPath (Join-Path $env:TEMP $name) -Destination (Join-Path $caseRoot $name) -Force}}
    }
    $case=[ordered]@{name=$caseName;requested_seconds=$Seconds;fps=$fps;width=1920;height=1080;audio=$false;video_profile=$VideoProfile;software_preset=$SoftwarePreset;profile_note=('x264 preset '+$SoftwarePreset+' / CRF23, or native NVENC p4 ll vbr CQ23 b:v0. Factory means X264Preset is omitted from the fresh INI. GIF uses product-specific lossless intermediate regardless of the video preset. Verify actual child command before claiming a match.');sharex_backend=$ShareXBackend;start_utc=$startUtc.ToString('o');wall_seconds=$processWall;timed_out=$timedOut;parent_pid=$process.Id;final_processes=$final;files=$probes;gpu_samples='recording_gpu_samples.jsonl';ffmpeg_commands=$childCommands;frame_drop_counter=$null;frame_drop_note='Product warning logs do not expose a reliable drop counter. Decode the fixture binary tick before drawing frame pacing conclusions.'}
    $report.cases+=@($case)
    Copy-Item -LiteralPath $caseRoot -Destination (Join-Path $OutputRoot $caseName) -Recurse
    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $OutputRoot 'recording_guest_results.json') -Encoding utf8
    Write-Output ($caseName+' completed, files '+$files.Count+', timeout '+$timedOut)
    Start-Sleep -Seconds 2
 }
} finally {
 if($process -and -not $process.HasExited){
    if($metrics){foreach($row in @($metrics.Sample()) | Where-Object alive){Stop-Process -Id $row.pid -Force -ErrorAction SilentlyContinue}}
    if(-not $process.HasExited){Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue}
 }
 if($metrics){$metrics.Close()}
 if($gpuJob){Stop-Job -Job $gpuJob -ErrorAction SilentlyContinue;Remove-Job -Job $gpuJob -Force -ErrorAction SilentlyContinue}
 if($fixture -and -not $fixture.HasExited){Stop-Process -Id $fixture.Id -Force -ErrorAction SilentlyContinue}
}

// Drives the untouched installed ShareX application's own GUI and recording manager.
// It does not implement recording. All recording/capture/encoding remains in ShareX and FFmpeg.
using System;
using System.IO;
using System.Reflection;
using System.Diagnostics;
using System.Drawing;
using System.Threading;
using System.Windows.Forms;

class RecordingShareX {
    static readonly BindingFlags All = BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance|BindingFlags.Static;
    static readonly Stopwatch Clock = Stopwatch.StartNew();
    static string folder, output, ffmpeg, backend, media, profile;
    static int fps, seconds;
    static Assembly app, capture;
    static object Get(object obj,string name) {
        Type t=obj as Type ?? obj.GetType(); object instance=obj is Type ? null:obj;
        var p=t.GetProperty(name,All); if(p!=null)return p.GetValue(instance);
        return t.GetField(name,All).GetValue(instance);
    }
    static void Set(object obj,string name,object value) {
        Type t=obj as Type ?? obj.GetType(); object instance=obj is Type ? null:obj;
        var p=t.GetProperty(name,All); var f=t.GetField(name,All);
        Type dest=p!=null?p.PropertyType:f.FieldType;
        if(dest.IsEnum)value=value is string ? Enum.Parse(dest,(string)value):Enum.ToObject(dest,value);
        if(p!=null)p.SetValue(instance,value);else f.SetValue(instance,value);
    }
    static void State(string text) {
        lock(Clock)File.AppendAllText(Path.Combine(output,"sharex_lifecycle.tsv"),Clock.Elapsed.TotalSeconds.ToString("F6",System.Globalization.CultureInfo.InvariantCulture)+"\t"+text+"\n");
    }
    static object Options() {
        object options=Activator.CreateInstance(capture.GetType("ShareX.ScreenCaptureLib.FFmpegOptions"));
        Set(options,"OverrideCLIPath",true); Set(options,"CLIPath",ffmpeg);
        Set(options,"VideoSource",backend);Set(options,"AudioSource","");
        Set(options,"VideoCodec","libx264");Set(options,"x264_Preset","veryfast");Set(options,"x264_CRF",23);
        Set(options,"GIFStatsMode","full");Set(options,"GIFDither","sierra2_4a");Set(options,"GIFBayerScale",2);
        if(profile.StartsWith("matched") && media=="video") {
            bool nvenc=profile=="matched-nvenc";
            if(nvenc)Set(options,"VideoCodec","h264_nvenc");
            string input=backend=="ddagrab" ? "-f lavfi -i ddagrab=output_idx=0:framerate=$fps$:video_size=$area_width$x$area_height$:offset_x=$area_x$:offset_y=$area_y$:draw_mouse=0"+(nvenc?"":" -vf hwdownload,format=bgra,format=yuv420p") : "-f gdigrab -framerate $fps$ -offset_x $area_x$ -offset_y $area_y$ -video_size $area_width$x$area_height$ -draw_mouse 0 -i desktop -vf format=yuv420p";
            string encoder=nvenc?"-c:v h264_nvenc -preset p4 -tune ll -rc vbr -cq 23 -b:v 0":"-c:v libx264 -preset veryfast -crf 23";
            Set(options,"UseCustomCommands",true);
            Set(options,"CustomCommands","-hide_banner -loglevel warning -y "+input+" "+encoder+" -t $duration$ -movflags +faststart \"$output$\"");
        }
        return options;
    }
    static object RecordingOptions(object ff) {
        object options=Activator.CreateInstance(capture.GetType("ShareX.ScreenCaptureLib.ScreenRecordingOptions"));
        Set(options,"IsRecording",true);Set(options,"IsLossless",media=="gif");Set(options,"FPS",fps);
        Set(options,"Duration",(float)seconds);Set(options,"CaptureArea",new Rectangle(0,0,1920,1080));
        Set(options,"DrawCursor",false);Set(options,"OutputPath",Path.Combine(output,"sharex-record.mp4"));Set(options,"FFmpeg",ff);
        return options;
    }
    static void Drive() {
        try {
            Type program=app.GetType("ShareX.Program"); Control form=null;
            for(int i=0;i<600;i++) {
                form=Get(program,"MainForm") as Control;
                if(form!=null && form.IsHandleCreated) {
                    try { if((bool)Get(form,"IsReady"))break; } catch {}
                }
                Thread.Sleep(50);
            }
            if(form==null || !form.IsHandleCreated)throw new Exception("ShareX main form did not become ready");
            State("full_app_ready"); Thread.Sleep(2000);
            Control ready=form;
            ready.Invoke((Action)(()=>{
                object task=Activator.CreateInstance(app.GetType("ShareX.TaskSettings"));
                foreach(string flag in new[]{"UseDefaultAfterCaptureJob","UseDefaultAfterUploadJob","UseDefaultGeneralSettings","UseDefaultCaptureSettings"})Set(task,flag,false);
                Set(task,"AfterCaptureJob",0);Set(task,"AfterUploadJob",0);
                Set(task,"OverrideScreenshotsFolder",true);Set(task,"ScreenshotsFolder",output);
                object general=Get(task,"GeneralSettings");
                Set(general,"PlaySoundAfterCapture",false);Set(general,"PlaySoundAfterUpload",false);Set(general,"PlaySoundAfterAction",false);Set(general,"ShowToastNotificationAfterTaskCompleted",false);
                object settings=Get(task,"CaptureSettings");
                Set(settings,"CaptureCustomRegion",new Rectangle(0,0,1920,1080));
                Set(settings,"ScreenRecordFPS",fps);Set(settings,"GIFFPS",fps);Set(settings,"ScreenRecordShowCursor",false);
                Set(settings,"ScreenRecordAutoStart",true);Set(settings,"ScreenRecordStartDelay",0f);
                Set(settings,"ScreenRecordFixedDuration",true);Set(settings,"ScreenRecordDuration",(float)seconds);
                Set(settings,"FFmpegOptions",Options());
                object planned=RecordingOptions(Get(settings,"FFmpegOptions"));
                File.WriteAllText(Path.Combine(output,"sharex_generated_command.txt"),(string)planned.GetType().GetMethod("GetFFmpegCommands").Invoke(planned,null));
                Type resultType=capture.GetType("ShareX.ScreenCaptureLib.ScreenRecordOutput");
                Type startType=app.GetType("ShareX.ScreenRecordStartMethod") ?? capture.GetType("ShareX.ScreenCaptureLib.ScreenRecordStartMethod");
                if(resultType==null)resultType=Type.GetType("ShareX.HelpersLib.ScreenRecordOutput, ShareX.HelpersLib");
                if(startType==null)startType=Type.GetType("ShareX.HelpersLib.ScreenRecordStartMethod, ShareX.HelpersLib");
                task=app.GetType("ShareX.TaskSettings").GetMethod("GetSafeTaskSettings",All).Invoke(null,new[]{task});
                MethodInfo start=app.GetType("ShareX.TaskHelpers").GetMethod("StartScreenRecording",All);
                var parameters=start.GetParameters();
                State("record_requested");
                start.Invoke(null,new[]{Enum.Parse(parameters[0].ParameterType,media=="gif"?"GIF":"FFmpeg"),Enum.Parse(parameters[1].ParameterType,"CustomRegion"),task});
            }));
            Type manager=app.GetType("ShareX.ScreenRecordManager"); bool seen=false;
            for(int i=0;i<(seconds+120)*10;i++) {
                bool active=(bool)Get(manager,"IsRecording");
                if(active&&!seen){seen=true;State("record_manager_active");}
                if(seen&&!active){State("record_manager_finished");break;}
                Thread.Sleep(100);
            }
            if(!seen)throw new Exception("ShareX recording manager never became active");
            if((bool)Get(manager,"IsRecording"))throw new Exception("ShareX recording manager timed out");
            Thread.Sleep(500);ready.BeginInvoke((Action)(()=>Application.Exit()));
        } catch(Exception e) {
            State("error "+e); Environment.Exit(4);
        }
    }
    [STAThread] static int Main(string[] args) {
        if(args.Length!=8)return 2;
        folder=AppContext.BaseDirectory;output=Path.GetFullPath(args[1]);ffmpeg=Path.GetFullPath(args[2]);
        fps=int.Parse(args[3]);seconds=int.Parse(args[4]);backend=args[5];media=args[6];profile=args[7];
        if(args[0]!="plan" && (!Environment.UserName.Equals("WDAGUtilityAccount",StringComparison.OrdinalIgnoreCase)||
            !(output.StartsWith(@"C:\ZipshotLab\output\",StringComparison.OrdinalIgnoreCase)||output.StartsWith(@"C:\ZipshotLab\work\",StringComparison.OrdinalIgnoreCase))))return 20;
        Directory.CreateDirectory(output);
        AppDomain.CurrentDomain.AssemblyResolve+=(s,e)=>{
            string name=new AssemblyName(e.Name).Name;
            string path=Path.Combine(folder,name=="ShareX"?"ShareX.original.dll":name+".dll");
            return File.Exists(path)?Assembly.LoadFrom(path):null;
        };
        capture=Assembly.LoadFrom(Path.Combine(folder,"ShareX.ScreenCaptureLib.dll"));
        if(args[0]=="plan"){
            object opts=RecordingOptions(Options());
            File.WriteAllText(Path.Combine(output,"sharex_generated_command.txt"),(string)opts.GetType().GetMethod("GetFFmpegCommands").Invoke(opts,null));
            return 0;
        }
        app=Assembly.LoadFrom(Path.Combine(folder,"ShareX.original.dll"));
        State("full_app_start");
        new Thread(Drive){IsBackground=true}.Start();
        app.GetType("ShareX.Program").GetMethod("Main",All).Invoke(null,new object[]{new[]{"-portable","-silent","-NoHotkeys","-multi"}});
        State("full_app_exit");return 0;
    }
}

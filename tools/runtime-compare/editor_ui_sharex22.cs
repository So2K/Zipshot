// Guest-only host for the unmodified ShareX 22.4.0 Avalonia editor library.
// Uses its actual EditorWindow, EditorView and annotated GetSnapshot path.
using System;
using System.IO;
using System.Reflection;
using System.Diagnostics;
using System.Threading;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Styling;
using Avalonia.Themes.Fluent;
using Avalonia.Threading;
using ShareX.ImageEditor.Integration;
using ShareX.ImageEditor.Presentation.Views;
using ShareX.ImageEditor.Core.Editor;
using SkiaSharp;

class EditorBenchApp:Application {
    public override void Initialize(){Styles.Add(new FluentTheme());RequestedThemeVariant=ThemeVariant.Dark;}
}

class EditorUiShareX22 {
    static string prefix,fixture;
    static EditorWindow window;
    static EditorView view;
    static EditorCore core;
    static Canvas annotationCanvas;
    static readonly List<object> events=new List<object>();
    static readonly BindingFlags Flags=BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance;
    static readonly CancellationTokenSource cancellation=new CancellationTokenSource();
    static bool ready,finishing;
    static long stopAt;
    [DllImport("kernel32.dll")]static extern bool ProcessIdToSessionId(uint pid,out uint session);
    [DllImport("user32.dll")]static extern bool GetWindowRect(IntPtr h,out RectNative r);
    [StructLayout(LayoutKind.Sequential)]struct RectNative{public int Left,Top,Right,Bottom;}
    static object Member(object obj,string name){if(obj==null)return null;var t=obj.GetType();var p=t.GetProperty(name,Flags);if(p!=null)return p.GetValue(obj);var f=t.GetField(name,Flags);if(f==null)throw new MissingMemberException(t.FullName,name);return f.GetValue(obj);}
    static bool Guest(){uint session;return Environment.UserName.Equals("WDAGUtilityAccount",StringComparison.OrdinalIgnoreCase)&&ProcessIdToSessionId((uint)Environment.ProcessId,out session)&&session!=0&&Environment.GetEnvironmentVariable("ZIPSHOT_RUNTIME_ISOLATED")=="1"&&File.Exists("C:/ZipshotLab/input/.isolation-approved.json");}
    static void Json(string suffix,object data){string path=prefix+suffix;File.WriteAllText(path+".writing",JsonSerializer.Serialize(data,new JsonSerializerOptions{WriteIndented=true}));File.Move(path+".writing",path,true);}
    static object Map(double x,double y){var p=annotationCanvas.PointToScreen(new Point(x,y));return new {x=p.X,y=p.Y};}
    static void State(string suffix){
        if(core==null||core.SourceImage==null||annotationCanvas==null)return;
        int w=core.SourceImage.Width,h=core.SourceImage.Height;
        var points=new List<object>();for(int i=0;i<120;i++)points.Add(new {time_ms=i*2000.0/119,point=Map(w*(.15+.7*i/119),h*(.5+.18*Math.Sin(i*6*Math.PI/119)))});
        var hwnd=window.TryGetPlatformHandle().Handle;RectNative rect;GetWindowRect(hwnd,out rect);
        Json(suffix,new {engine="ShareX actual Avalonia EditorWindow",assembly_version=typeof(EditorWindow).Assembly.GetName().Version.ToString(),runtime_version=Environment.Version.ToString(),scope="Guest readiness and input coordinates, not proof of a visible frame",pid=Environment.ProcessId,session_id=Process.GetCurrentProcess().SessionId,hwnd=hwnd.ToInt64().ToString("x"),qpc_frequency=Stopwatch.Frequency,window=new[]{rect.Left,rect.Top,rect.Right,rect.Bottom},image_size=new[]{w,h},canvas_bounds=new[]{annotationCanvas.Bounds.Width,annotationCanvas.Bounds.Height},source_origin=Map(0,0),source_x1000=Map(1000,0),source_y1000=Map(0,1000),arrow=new {from=Map(w*.2,h*.75),to=Map(w*.8,h*.25)},freehand=points,blur=new{from=Map(w*.25,h*.3),to=Map(w*.75,h*.7)},annotation_count=core.Annotations.Count,can_undo=core.CanUndo,can_redo=core.CanRedo});
    }
    static void Pointer(string kind,PointerEventArgs e){var p=annotationCanvas==null?new Point():e.GetPosition(annotationCanvas);events.Add(new{kind,qpc=Stopwatch.GetTimestamp(),x=p.X,y=p.Y});}
    static void Snapshot(){
        var get=view.GetType().GetMethod("GetSnapshot",Flags,null,Type.EmptyTypes,null);
        using(var bitmap=(SKBitmap)get.Invoke(view,null)){
            if(bitmap==null)throw new Exception("Actual annotated EditorView.GetSnapshot returned null");
            using(var image=SKImage.FromBitmap(bitmap))using(var data=image.Encode(SKEncodedImageFormat.Png,100))using(var output=File.Create(prefix+"-snapshot.png"))data.SaveTo(output);
            Json("-snapshot.json",new {method="ShareX 22.4.0 EditorView.GetSnapshot",width=bitmap.Width,height=bitmap.Height,annotation_count=core.Annotations.Count,qpc=Stopwatch.GetTimestamp()});
        }
    }
    static void Finish(){
        if(finishing)return;finishing=true;
        try{State("-final.json");Snapshot();Json("-events.json",new {scope="Avalonia routed input receipt timestamps only. No inferred first-visible timestamp or paint latency.",qpc_frequency=Stopwatch.Frequency,events});}
        catch(Exception error){Json("-error.json",new{error=error.ToString()});}
        window.Close();cancellation.Cancel();
    }
    [STAThread]static int Main(string[] args){
        // Before Avalonia platform initialization or native window creation.
        if(!Guest()){Console.Error.WriteLine("Refused outside approved Windows Sandbox identity, session, marker and opt-in.");return 20;}
        if(args.Length!=5){Console.Error.WriteLine("fixture output-prefix width height lifetime-ms");return 2;}
        fixture=args[0];prefix=args[1];int width=int.Parse(args[2]),height=int.Parse(args[3]),life=int.Parse(args[4]);
        if(width<640||height<480||life<1000||life>300000)return 2;
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(prefix)));
        try{return Run(width,height,life);}catch(Exception error){Json("-error.json",new{error=error.ToString()});return 10;}
    }
    static int Run(int width,int height,int life){
        AppBuilder.Configure<EditorBenchApp>().UsePlatformDetect().SetupWithoutStarting();
        ImageEditorIntegration.Initialize();
        var options=new ImageEditorOptions{RememberWindowState=false,IsWindowMaximized=false,WindowWidth=width,WindowHeight=height,ShowExitConfirmation=false,ZoomToFitOnOpen=true,AutoCopyImageToClipboard=false,AutoCloseEditorOnTask=false,ShowNotifications=false,AutoExpandCanvas=false,Thickness=4,BorderColorHex="#FFFF0000",Shadow=false,BlurStrength=15};
        window=new EditorWindow(options);window.Width=width;window.Height=height;window.Position=new PixelPoint(40,40);
        window.AddHandler(InputElement.PointerPressedEvent,(object s,PointerPressedEventArgs e)=>Pointer("pointer_down",e),RoutingStrategies.Tunnel,true);
        window.AddHandler(InputElement.PointerMovedEvent,(object s,PointerEventArgs e)=>Pointer("pointer_move",e),RoutingStrategies.Tunnel,true);
        window.AddHandler(InputElement.PointerReleasedEvent,(object s,PointerReleasedEventArgs e)=>Pointer("pointer_up",e),RoutingStrategies.Tunnel,true);
        window.AddHandler(InputElement.PointerWheelChangedEvent,(object s,PointerWheelEventArgs e)=>{Pointer("wheel",e);},RoutingStrategies.Tunnel,true);
        window.AddHandler(InputElement.KeyDownEvent,(object s,KeyEventArgs e)=>events.Add(new{kind="key_down",qpc=Stopwatch.GetTimestamp(),key=e.Key.ToString(),modifiers=e.KeyModifiers.ToString()}),RoutingStrategies.Tunnel,true);
        window.AddHandler(InputElement.KeyUpEvent,(object s,KeyEventArgs e)=>events.Add(new{kind="key_up",qpc=Stopwatch.GetTimestamp(),key=e.Key.ToString(),modifiers=e.KeyModifiers.ToString()}),RoutingStrategies.Tunnel,true);
        window.LoadImage(fixture);window.Show();
        var timer=new DispatcherTimer{Interval=TimeSpan.FromMilliseconds(100)};
        timer.Tick+=(s,e)=>{
            if(view==null)view=(EditorView)Member(window,"EditorViewControl");
            if(view!=null){core=(EditorCore)Member(view,"EditorCore");annotationCanvas=(Canvas)Member(view,"AnnotationCanvas");}
            if(!ready&&core!=null&&core.SourceImage!=null&&annotationCanvas!=null&&annotationCanvas.Bounds.Width>0&&annotationCanvas.Bounds.Height>0){ready=true;State("-ready.json");stopAt=Stopwatch.GetTimestamp()+(long)(life/1000.0*Stopwatch.Frequency);}
            string command=prefix+"-command.txt";
            if(File.Exists(command)){
                string action=File.ReadAllText(command).Trim();File.Delete(command);
                try{if(action=="state")State("-state.json");else if(action=="snapshot")Snapshot();else if(action=="stop")Finish();else Json("-command-error.json",new{error="Unknown command",action});}
                catch(Exception error){Json("-command-error.json",new{error=error.ToString()});}
            }
            if(ready&&Stopwatch.GetTimestamp()>=stopAt)Finish();
        };
        timer.Start();Dispatcher.UIThread.MainLoop(cancellation.Token);timer.Stop();return 0;
    }
}

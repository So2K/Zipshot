// Exercises the installed, unmodified ShareX 18.0.1 shape export implementation.
// A data-only ShapeManager is allocated without its UI constructor. No form,
// desktop, clipboard, pointer state or installed settings are accessed.
using System;
using System.IO;
using System.Drawing;
using System.Drawing.Imaging;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using ShareX.ScreenCaptureLib;

class EditorExportBench {
    [DllImport("kernel32.dll")] static extern bool SetProcessAffinityMask(IntPtr process,UIntPtr mask);
    [DllImport("kernel32.dll")] static extern bool SetPriorityClass(IntPtr process,uint priority);
    const BindingFlags Flags=BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance;
    static string lib;
    static Assembly Resolve(object sender,ResolveEventArgs e) {
        string p=Path.Combine(lib,new AssemblyName(e.Name).Name+".dll");
        return File.Exists(p)?Assembly.LoadFrom(p):null;
    }
    static void Set(object target,string property,object value) {
        var p=target.GetType().GetProperty(property,Flags);
        if(p==null) throw new MissingMemberException(target.GetType().FullName,property);
        if(p.PropertyType.IsEnum) value=Enum.ToObject(p.PropertyType,value);
        p.SetValue(target,value);
    }
    static void Style(BaseDrawingShape s,object manager) {
        typeof(BaseShape).GetProperty("Manager",Flags).SetValue(s,manager);
        s.BorderColor=Color.Red; s.BorderSize=4; s.FillColor=Color.Transparent; s.Shadow=false;
    }
    static List<BaseShape> Configure(string name,object manager,int w,int h) {
        var shapes=new List<BaseShape>();
        Action<float,float,float,float> arrow=(x0,y0,x1,y1)=>{
            var s=new ArrowDrawingShape(); Style(s,manager);
            s.Points[0]=new PointF(x0,y0); s.Points[1]=new PointF(x1,y1);
            s.Rectangle=RectangleF.FromLTRB(Math.Min(x0,x1),Math.Min(y0,y1),Math.Max(x0,x1),Math.Max(y0,y1));
            shapes.Add(s);
        };
        if(name=="arrow") arrow(w*.2f,h*.75f,w*.8f,h*.25f);
        else if(name=="arrows32") {
            for(int i=0;i<32;i++){float y=h*(.12f+.72f*i/31);arrow(w*.15f,y,w*.85f,h-y);}
        } else if(name=="freehand") {
            var s=new FreehandDrawingShape(); Style(s,manager);
            var points=new List<PointF>();
            for(int i=0;i<120;i++) points.Add(new PointF(w*(.15f+.7f*i/119),h*(.5f+.18f*(float)Math.Sin(i*6*Math.PI/119))));
            typeof(FreehandDrawingShape).GetField("positions",Flags).SetValue(s,points);
            s.Rectangle=new RectangleF(w*.15f,h*.32f,w*.7f,h*.36f); shapes.Add(s);
        } else if(name=="blur_cold"||name=="blur_warm") {
            var s=new BlurEffectShape();
            typeof(BaseShape).GetProperty("Manager",Flags).SetValue(s,manager);
            s.Rectangle=new RectangleF(w*.25f,h*.3f,w*.5f,h*.4f); s.BlurRadius=15; shapes.Add(s);
        } else if(name!="plain") throw new ArgumentException("Unknown export case");
        return shapes;
    }
    [STAThread]
    static int Main(string[] args) {
        if(args.Length!=6){Console.Error.WriteLine("lib fixture case warmups samples output.png");return 2;}
        lib=args[0]; AppDomain.CurrentDomain.AssemblyResolve+=Resolve;
        if(!SetProcessAffinityMask(Process.GetCurrentProcess().Handle,new UIntPtr(15))) return 8;
        SetPriorityClass(Process.GetCurrentProcess().Handle,0x20);
        try{return Run(args);}catch(Exception e){Console.Error.WriteLine(e);return 10;}
    }
    static int Run(string[] args) {
        string name=args[2];int warm=int.Parse(args[3]),count=int.Parse(args[4]);
        if(warm<0||count<1||count>10000)return 2;
        var assembly=typeof(ArrowDrawingShape).Assembly;
        var type=assembly.GetType("ShareX.ScreenCaptureLib.ShapeManager",true);
        // Exact export method uses Shapes and DPI; UI events/state are deliberately outside this microbenchmark.
        object manager=RuntimeHelpers.GetUninitializedObject(type);
        type.GetField("CurrentDPI",Flags).SetValue(manager,new PointF(96,96));
        var render=type.GetMethod("RenderOutputImage",Flags,null,new[]{typeof(Bitmap)},null);
        using(var loaded=new Bitmap(args[1]))
        using(var source=new Bitmap(loaded.Width,loaded.Height,PixelFormat.Format32bppArgb)) {
            source.SetResolution(96,96);
            using(var g=Graphics.FromImage(source))g.DrawImageUnscaled(loaded,0,0);
            var shapes=Configure(name,manager,source.Width,source.Height);Set(manager,"Shapes",shapes);
            var call=new object[]{source}; var samples=new List<double>(); Bitmap output=null;
            for(int i=-warm;i<count;i++) {
                long a=Stopwatch.GetTimestamp();
                var next=(Bitmap)render.Invoke(manager,call);
                long b=Stopwatch.GetTimestamp();
                if(next==null||next.Width!=source.Width||next.Height!=source.Height)return 4;
                if(output!=null)output.Dispose();output=next;
                if(i>=0)samples.Add((b-a)*1000.0/Stopwatch.Frequency);
            }
            using(output) {
                long changed=0;ulong signature=1469598103934665603UL;
                var rect=new Rectangle(0,0,source.Width,source.Height);
                var a=source.LockBits(rect,ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);
                var b=output.LockBits(rect,ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);
                var rowA=new int[source.Width];var rowB=new int[source.Width];
                for(int y=0;y<source.Height;y++) {
                    Marshal.Copy(IntPtr.Add(a.Scan0,y*a.Stride),rowA,0,rowA.Length);
                    Marshal.Copy(IntPtr.Add(b.Scan0,y*b.Stride),rowB,0,rowB.Length);
                    for(int x=0;x<rowA.Length;x++){if(rowA[x]!=rowB[x])changed++;signature=unchecked((signature^(uint)rowB[x])*1099511628211UL);}
                }
                source.UnlockBits(a);output.UnlockBits(b);
                if(name!="plain"&&changed==0)return 5;
                if(name=="plain"&&changed!=0)return 6;
                output.Save(args[5],ImageFormat.Png);
                Console.Write("{\"engine\":\"ShareX production ShapeManager.RenderOutputImage\",\"scope\":\"render_export_only_not_UI_latency\",\"assembly_version\":\""+assembly.GetName().Version+"\",\"runtime_version\":\""+Environment.Version+"\",\"width\":"+source.Width+",\"height\":"+source.Height+",\"case\":\""+name+"\",\"warmups\":"+warm+",\"samples\":"+count+",\"changed_pixels\":"+changed+",\"pixel_signature\":\""+signature.ToString("x16")+"\",\"ms\":[");
                for(int i=0;i<samples.Count;i++)Console.Write((i==0?"":",")+samples[i].ToString("F6",CultureInfo.InvariantCulture));
                Console.WriteLine("]}");
            }
            foreach(var s in shapes)s.Dispose();
        }
        GC.SuppressFinalize(manager);return 0;
    }
}

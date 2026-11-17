// Calls the installed ShareX screenshot API; all pixels remain in memory and are disposed.
using System;
using System.IO;
using System.Drawing;
using System.Reflection;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;

class CaptureBench {
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int value);
    static string lib;
    static Assembly Resolve(object sender, ResolveEventArgs e) {
        string path=Path.Combine(lib,new AssemblyName(e.Name).Name+".dll");
        return File.Exists(path) ? Assembly.LoadFrom(path) : null;
    }
    static int Main(string[] args) {
        if(args.Length!=3) return 2;
        SetProcessDpiAwarenessContext(new IntPtr(-4));
        lib=args[0];
        AppDomain.CurrentDomain.AssemblyResolve+=Resolve;
        return Run(args);
    }
    static int Run(string[] args) {
        var assembly=Assembly.LoadFrom(Path.Combine(lib,"ShareX.ScreenCaptureLib.dll"));
        var type=assembly.GetType("ShareX.ScreenCaptureLib.Screenshot");
        object screenshot=Activator.CreateInstance(type);
        type.GetProperty("CaptureCursor").SetValue(screenshot,false);
        var capture=type.GetMethod("CaptureRectangle",new Type[]{typeof(Rectangle)});
        Rectangle r=new Rectangle(GetSystemMetrics(76),GetSystemMetrics(77),GetSystemMetrics(78),GetSystemMetrics(79));
        object[] call={r};
        int warm=int.Parse(args[1]),count=int.Parse(args[2]);
        Console.Write("{\"engine\":\"ShareX Screenshot.CaptureRectangle\",\"library_version\":\""+assembly.GetName().Version+"\",\"runtime_version\":\""+Environment.Version+"\",\"x\":"+r.X+",\"y\":"+r.Y+",\"width\":"+r.Width+",\"height\":"+r.Height+",\"cursor\":false,\"ms\":[");
        for(int i=-warm;i<count;i++) {
            long a=Stopwatch.GetTimestamp();
            using(var img=(Bitmap)capture.Invoke(screenshot,call)) {
                long b=Stopwatch.GetTimestamp();
                if(img==null || img.Width!=r.Width || img.Height!=r.Height) return 3;
                if(i>=0) Console.Write((i==0?"":",")+((b-a)*1000.0/Stopwatch.Frequency).ToString("F6",CultureInfo.InvariantCulture));
            }
        }
        Console.WriteLine("]}");
        return 0;
    }
}

// Executes the official ShareX 22.4.0 TaskHelpers/SkiaImageHelpers/ImageData paths.
// Own entry point only: no application startup, UI, hooks, clipboard or settings IO.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text.Json;
using ShareX;
using ShareX.HelpersLib;
using SkiaSharp;

[assembly: SupportedOSPlatform("windows10.0.22621")]

static class ImageBenchmark {
    static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    static double Clock() => Stopwatch.GetTimestamp() * 1000.0 / Stopwatch.Frequency;
    static byte[] Straight(SKBitmap bitmap) {
        using var converted = new SKBitmap(new SKImageInfo(bitmap.Width, bitmap.Height, SKColorType.Bgra8888, SKAlphaType.Unpremul));
        using var pixels = bitmap.PeekPixels();
        if (!pixels.ReadPixels(converted.Info, converted.GetPixels(), converted.RowBytes)) throw new Exception("ReadPixels");
        return converted.Bytes;
    }
    static object Member(object instance, string name) {
        const BindingFlags f=BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance;
        var type=instance.GetType();return type.GetField(name,f)?.GetValue(instance)??type.GetProperty(name,f)?.GetValue(instance);
    }
    static void Set(object instance,string name,object value) {
        const BindingFlags f=BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance;
        var type=instance.GetType();var field=type.GetField(name,f);
        if(field!=null)field.SetValue(instance,value);else type.GetProperty(name,f).SetValue(instance,value);
    }
    static void InitSettings() {
        var state=typeof(TaskSettings).Assembly.GetType("ShareX.ApplicationState",true);
        const BindingFlags flags=BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Static;
        var config=new ApplicationConfig();var field=state.GetField("Settings",flags);
        if(field!=null)field.SetValue(null,config);else state.GetProperty("Settings",flags).SetValue(null,config);
    }
    static int Main(string[] args) {
        CultureInfo.CurrentCulture=CultureInfo.InvariantCulture;
        if(args.Length!=5&&args.Length!=6)return 2;
        using(var process=Process.GetCurrentProcess()){process.PriorityClass=ProcessPriorityClass.BelowNormal;process.ProcessorAffinity=(IntPtr)15;}
        var folder=Path.GetFullPath(args[0]);int width=int.Parse(args[1]),height=int.Parse(args[2]),n=int.Parse(args[4]);
        bool alpha=bool.Parse(args[3]);InitSettings();
        bool filesOnly=args.Length==6&&args[5]=="--file-only";
        if(args.Length==6&&!filesOnly){
            string input=Path.GetFullPath(args[5]);Directory.CreateDirectory(folder);
            using var decodeCsv=new StreamWriter(Path.Combine(folder,"sharex-samefile-decode.csv"));
            decodeCsv.WriteLine("file,engine,phase,trial,ms,bytes,decoded_sha256,runtime");
            for(int trial=-1;trial<n;trial++){
                double begin=Clock();using var decoded=SkiaImageHelpers.LoadImage(input);double ms=Clock()-begin;
                if(decoded==null)throw new Exception("Official same-file decode failed");
                decodeCsv.WriteLine($"{input.Replace('\\','/')},sharex-22.4.0,{(trial<0?"first":"warm")},{trial},{ms},{new FileInfo(input).Length},{Hash(Straight(decoded))},{Environment.Version}");
            }
            Console.WriteLine($"PASS ShareX same-file decoder n={n}");return 0;
        }
        byte[] straight=File.ReadAllBytes(Path.Combine(folder,"fixture.bgra"));
        if(straight.Length!=width*height*4)return 3;
        var premul=(byte[])straight.Clone();
        if(alpha)for(int i=0;i<premul.Length;i+=4){int a=premul[i+3];for(int c=0;c<3;++c)premul[i+c]=(byte)((premul[i+c]*a+127)/255);}
        using var bitmap=SkiaImageHelpers.CreateBitmap(width,height); // Actual app: BGRA8888/Premul.
        Marshal.Copy(premul,0,bitmap.GetPixels(),premul.Length);
        var task=new TaskSettings();
        var settings=Member(task,"ImageSettings");
        var fields=new[]{"ImageFormat","ImagePNGBitDepth","ImagePNGCompressionLevel","ImagePNGFilter","ImageJPEGQuality","ImageJPEGSubsampling","ImageAutoUseJPEG","ImageAutoUseJPEGSize","ImageAutoJPEGQuality"};
        File.WriteAllText(Path.Combine(folder,"sharex-defaults.json"),JsonSerializer.Serialize(new{
            assembly=typeof(TaskHelpers).Assembly.GetName().Version.ToString(),runtime=Environment.Version.ToString(),
            input_representation="actual CreateBitmap BGRA8888/Premul; alpha premultiplication performed outside timing",
            settings=fields.ToDictionary(k=>k,k=>Member(settings,k)?.ToString()),
            source_straight_sha256=Hash(straight),source_premul_sha256=Hash(premul)
        },new JsonSerializerOptions{WriteIndented=true}));
        using var csv=new StreamWriter(Path.Combine(folder,"sharex.csv"));
        csv.WriteLine("engine,operation,phase,trial,ms,bytes,source_sha256,encoded_sha256,decoded_sha256,format");
        foreach(bool jpeg in new[]{false,true}) {
            if(filesOnly)break;
            if(jpeg&&alpha)continue;
            var format=jpeg?EImageFormat.JPEG:EImageFormat.PNG;
            string output=Path.Combine(folder,jpeg?"sharex.jpg":"sharex.png");
            for(int trial=-1;trial<n;trial++) {
                double start=Clock();using var stream=TaskHelpers.SaveImageAsStream(bitmap,format,task);double ms=Clock()-start;
                string encoded="",decoded="";
                if(trial==-1){File.WriteAllBytes(output,stream.ToArray());encoded=Hash(stream.ToArray());using var check=SkiaImageHelpers.LoadImage(output);decoded=Hash(Straight(check));}
                csv.WriteLine($"sharex-22.4.0,{(jpeg?"jpeg":"png")}-encode-memory,{(trial<0?"first":"warm")},{trial},{ms},{stream.Length},{Hash(straight)},{encoded},{decoded},{format}");
            }
            string input=Path.Combine(folder,jpeg?"engine0.jpg":"engine0.png");
            for(int trial=-1;trial<n;trial++) {
                double start=Clock();using var decoded=SkiaImageHelpers.LoadImage(input);double ms=Clock()-start;
                if(decoded==null)throw new Exception("Official decode failed");
                csv.WriteLine($"sharex-22.4.0,{(jpeg?"jpeg":"png")}-decode-file,{(trial<0?"first":"warm")},{trial},{ms},{new FileInfo(input).Length},{Hash(straight)},,{Hash(Straight(decoded))},{format}");
            }
        }
        foreach(bool matched in new[]{false,true}) {
            if(filesOnly&&matched)continue;
            task=new TaskSettings();settings=Member(task,"ImageSettings");
            if(matched){Set(settings,"ImageAutoUseJPEG",true);Set(settings,"ImageAutoUseJPEGSize",2048);Set(settings,"ImageAutoJPEGQuality",false);Set(settings,"ImageJPEGQuality",90);}
            for(int trial=-1;trial<n;trial++) {
                string output=Path.Combine(folder,"sharex-save.tmp");
                double start=Clock();using var data=TaskHelpers.PrepareImage(bitmap,task);if(!data.Write(output))throw new Exception("Official write failed");double ms=Clock()-start;
                csv.WriteLine($"sharex-22.4.0,{(matched?"save-auto-matched-2048000B-q90":"save-auto-default")},{(trial<0?"first":"warm")},{trial},{ms},{new FileInfo(output).Length},{Hash(straight)},,,{data.ImageFormat}");
                File.Delete(output);
            }
        }
        for(int trial=-1;trial<n;trial++) {
            string output=Path.Combine(folder,"sharex-forced-png.tmp");
            task=new TaskSettings();
            double begin=Clock();
            using var stream=TaskHelpers.SaveImageAsStream(bitmap,EImageFormat.PNG,task);
            using var data=new ImageData{ImageStream=stream,ImageFormat=EImageFormat.PNG};
            if(!data.Write(output))throw new Exception("Official forced PNG write failed");
            double ms=Clock()-begin;
            csv.WriteLine($"sharex-22.4.0,png-encode-file-forced,{(trial<0?"first":"warm")},{trial},{ms},{new FileInfo(output).Length},{Hash(straight)},,,PNG");
            File.Delete(output);
        }
        Console.WriteLine($"PASS ShareX {width}x{height} alpha={alpha} n={n}");return 0;
    }
}

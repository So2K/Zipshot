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
public static class RecordingFixtureEntry {
 [System.Runtime.InteropServices.DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(System.IntPtr value);
 [System.STAThread] public static int Main(string[] args) {
  if(System.Environment.UserName!="WDAGUtilityAccount" || args.Length!=1)return 20;
  string output=System.IO.Path.GetFullPath(args[0]);
  if(!(output.StartsWith(@"C:\ZipshotLab\output\",System.StringComparison.OrdinalIgnoreCase)||output.StartsWith(@"C:\ZipshotLab\work\",System.StringComparison.OrdinalIgnoreCase)))return 21;
  SetProcessDpiAwarenessContext(new System.IntPtr(-4));
  System.Windows.Forms.Application.Run(new RecordingFixture(output)); return 0;
 }
}

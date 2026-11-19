// Metadata/API inventory only. No app/editor instance or UI is created.
using System;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Collections.Generic;
class EditorApiProbe {
    static string lib;
    static Assembly Resolve(object sender,ResolveEventArgs e){string p=Path.Combine(lib,new AssemblyName(e.Name).Name+".dll");return File.Exists(p)?Assembly.LoadFrom(p):null;}
    static void Calls(MethodInfo method){
        var body=method.GetMethodBody();if(body==null)return;var il=body.GetILAsByteArray();
        var ops=new Dictionary<short,OpCode>();foreach(var f in typeof(OpCodes).GetFields())if(f.FieldType==typeof(OpCode)){var op=(OpCode)f.GetValue(null);ops[op.Value]=op;}
        for(int i=0;i<il.Length;){int offset=i;short code=il[i++];if(code==0xfe)code=(short)(0xfe00|il[i++]);var op=ops[code];int n=0;
            switch(op.OperandType){case OperandType.InlineNone:break;case OperandType.ShortInlineBrTarget:case OperandType.ShortInlineI:case OperandType.ShortInlineVar:n=1;break;case OperandType.InlineVar:n=2;break;case OperandType.InlineI8:case OperandType.InlineR:n=8;break;case OperandType.InlineSwitch:n=4+4*BitConverter.ToInt32(il,i);break;default:n=4;break;}
            if(op.OperandType==OperandType.InlineMethod||op.OperandType==OperandType.InlineField||op.OperandType==OperandType.InlineType||op.OperandType==OperandType.InlineTok){try{var member=method.Module.ResolveMember(BitConverter.ToInt32(il,i));Console.WriteLine(" IL "+offset+" "+op.Name+" "+member.DeclaringType+"."+member);}catch{}}
            i+=n;
        }
    }
    static int Main(string[] args){
        lib=args[0];AppDomain.CurrentDomain.AssemblyResolve+=Resolve;
        var a=Assembly.LoadFrom(Path.Combine(lib,"ShareX.ImageEditor.dll"));
        Console.WriteLine("ASSEMBLY "+a.FullName+" RUNTIME "+Environment.Version);
        foreach(string name in new[]{"Core.Editor.EditorCore","Core.Annotations.Annotation","Core.Annotations.ArrowAnnotation","Core.Annotations.FreehandAnnotation","Core.Annotations.BlurAnnotation","Core.History.EditorHistory","Integration.ImageEditorIntegration","Integration.ImageEditorOptions","Presentation.Views.EditorView","Presentation.Views.EditorWindow"}){
            var t=a.GetType("ShareX.ImageEditor."+name,true);
            Console.WriteLine("TYPE "+t.FullName);
            var flags=BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance|BindingFlags.Static|BindingFlags.DeclaredOnly;
            foreach(var c in t.GetConstructors(flags))Console.WriteLine("CTOR "+c);
            foreach(var p in t.GetProperties(flags))Console.WriteLine("PROPERTY "+p);
            foreach(var f in t.GetFields(flags))if(!f.Name.Contains("BackingField"))Console.WriteLine("FIELD "+f);
            foreach(var m in t.GetMethods(flags))if(!m.IsSpecialName){Console.WriteLine("METHOD "+m);if(name=="Integration.ImageEditorIntegration"){foreach(var p in m.GetParameters())Console.WriteLine(" ARG "+p.Name+" "+p.ParameterType+" default "+p.DefaultValue);Calls(m);}else if(m.Name=="GetSnapshot")Calls(m);}
        }
        return 0;
    }
}

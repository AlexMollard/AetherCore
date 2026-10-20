using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Diagnostics.CodeAnalysis;
using System.Text;

[StructLayout(LayoutKind.Sequential)]
internal struct Vec3 { public float X, Y, Z; }

internal sealed class ProbeScript
{
    public int Score = 7;
    public void Update() { Score++; }
}

internal static unsafe class Program
{
    [DllImport("probe")] private static extern int aether_probe_scalar(int value);
    [DllImport("probe")] private static extern int aether_probe_utf8(byte* value, uint length);
    [DllImport("probe")] private static extern void aether_probe_vec3(Vec3 value, Vec3* result);
    [DllImport("probe")] private static extern Vec3 aether_probe_return_vec3();
    [DllImport("probe")] private static extern int aether_probe_ticks(delegate* unmanaged[Cdecl]<int, int> callback, int ticks);
    private static ProbeScript s_script = new();
    private static bool s_caught;

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int Tick(int value)
    {
        if (value == 10)
        {
            try { throw new InvalidOperationException("Recoverable gameplay error"); }
            catch (InvalidOperationException) { s_caught = true; }
        }
        s_script.Update();
        return s_script.Score;
    }

    [DynamicDependency(DynamicallyAccessedMemberTypes.PublicFields, typeof(ProbeScript))]
    private static int Main()
    {
        bool scalar = aether_probe_scalar(20) == 41;
        byte[] text = Encoding.UTF8.GetBytes("caf\u00e9\0agent");
        bool utf8;
        fixed (byte* bytes = text) { utf8 = aether_probe_utf8(bytes, (uint)text.Length) == 1; }
        Vec3 result;
        aether_probe_vec3(new Vec3 { X = 2, Y = 3, Z = 5 }, &result);
        bool structure = result.X == 3 && result.Y == 5 && result.Z == 8;
        result = aether_probe_return_vec3();
        structure = structure && result.X == 3 && result.Y == 5 && result.Z == 8;
        bool callbacks = aether_probe_ticks(&Tick, 60) == 67;
        bool reflection = (int)typeof(ProbeScript).GetField("Score")!.GetValue(s_script)! == 67;
        callbacks = callbacks && s_caught;
        bool passed = scalar && utf8 && structure && callbacks && reflection;
        Console.WriteLine("{\"profile\":\"csharp-wasm-interop-v1\",\"status\":\"" + (passed ? "passed" : "failed") + "\",\"runtime\":\"" + Environment.Version + "\",\"scalar\":" + scalar.ToString().ToLowerInvariant() + ",\"utf8\":" + utf8.ToString().ToLowerInvariant() + ",\"structure\":" + structure.ToString().ToLowerInvariant() + ",\"callbacks\":" + callbacks.ToString().ToLowerInvariant() + ",\"reflection\":" + reflection.ToString().ToLowerInvariant() + "}");
        return passed ? 0 : 1;
    }
}

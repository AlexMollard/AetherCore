using System;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;
using System.Text.Json;
using Xunit;

namespace AetherCore.Tests;

public sealed class ScriptCompilerTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void RoslynOutputHasRelocatableRuntimeDependencyManifest(bool optimize)
    {
        string root = Path.Combine(Path.GetTempPath(), "aether-compile-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            string source = Path.Combine(root, "source");
            Directory.CreateDirectory(source);
            File.WriteAllText(Path.Combine(source, "Player.cs"),
                "public sealed class Player : AetherCore.EntityScript { public int Answer() => 42; }");
            string referenceDir = Path.Combine(root, "installed-sdk");
            Directory.CreateDirectory(referenceDir);
            string sdk = Path.Combine(referenceDir, "AetherCore.dll");
            File.Copy(typeof(EntityScript).Assembly.Location, sdk);
            byte[] sdkBefore = File.ReadAllBytes(sdk);
            string output = Path.Combine(root, "build", "AetherGame.dll");
            Type compiler = Assembly.Load("AetherCore.Interop").GetType("AetherCore.Interop.ScriptCompiler", true)!;
            MethodInfo compile = compiler.GetMethod("Compile", BindingFlags.Static | BindingFlags.NonPublic)!;
            object?[] args = [source, output, referenceDir, optimize, null];
            Assert.True((int)compile.Invoke(null, args)! == 0, args[4]?.ToString());
            Assert.Equal(new[] { sdk }, Directory.GetFiles(referenceDir));
            Assert.Equal(sdkBefore, File.ReadAllBytes(sdk));
            Assert.True(File.Exists(Path.Combine(Path.GetDirectoryName(output)!, "AetherCore.dll")));
            byte[] firstGame = File.ReadAllBytes(output);
            string otherSource = Path.Combine(root, "other-project");
            Directory.CreateDirectory(otherSource);
            File.WriteAllText(Path.Combine(otherSource, "Other.cs"), "public sealed class Other : AetherCore.EntityScript { }");
            string otherOutput = Path.Combine(root, "other-output", "AetherGame.dll");
            object?[] otherArgs = [otherSource, otherOutput, referenceDir, optimize, null];
            Assert.True((int)compile.Invoke(null, otherArgs)! == 0, otherArgs[4]?.ToString());
            Assert.Equal(firstGame, File.ReadAllBytes(output));
            Assert.Equal(new[] { sdk }, Directory.GetFiles(referenceDir));
            Assert.Equal(sdkBefore, File.ReadAllBytes(sdk));

            string manifest = Path.ChangeExtension(output, ".deps.json");
            Assert.True(File.Exists(manifest));
            using JsonDocument deps = JsonDocument.Parse(File.ReadAllText(manifest));
            string target = deps.RootElement.GetProperty("runtimeTarget").GetProperty("name").GetString()!;
            Assert.StartsWith(".NETCoreApp,Version=v", target);
            Assert.DoesNotContain(root, File.ReadAllText(manifest), StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("Microsoft.CodeAnalysis", File.ReadAllText(manifest), StringComparison.Ordinal);
            string gameKey = "AetherGame/" + AssemblyName.GetAssemblyName(output).Version;
            Assert.Equal(typeof(EntityScript).Assembly.GetName().Version!.ToString(),
                deps.RootElement.GetProperty("targets").GetProperty(target).GetProperty(gameKey)
                    .GetProperty("dependencies").GetProperty("AetherCore").GetString());

            string delivered = Path.Combine(root, "relocated");
            Directory.CreateDirectory(delivered);
            foreach (string file in Directory.GetFiles(Path.GetDirectoryName(output)!))
            {
                File.Copy(file, Path.Combine(delivered, Path.GetFileName(file)));
            }
            // No source, SDK, NuGet cache path or compiler is needed to resolve the bundle.
            Directory.Delete(source, true);
            Directory.Delete(Path.GetDirectoryName(output)!, true);
            Directory.Delete(referenceDir, true);
            string game = Path.Combine(delivered, "AetherGame.dll");
            AssemblyDependencyResolver resolver = new(game);
            Assert.Equal(Path.Combine(delivered, "AetherCore.dll"), resolver.ResolveAssemblyToPath(typeof(EntityScript).Assembly.GetName()));
            AssemblyLoadContext context = new("delivered-script-test", isCollectible: true);
            context.Resolving += (_, name) => name.Name == "AetherCore" ? typeof(EntityScript).Assembly : null;
            try
            {
                using FileStream gameBytes = File.OpenRead(game);
                Type player = context.LoadFromStream(gameBytes).GetType("Player", true)!;
                Assert.IsAssignableFrom<EntityScript>(Activator.CreateInstance(player));
                Assert.Equal(42, player.GetMethod("Answer")!.Invoke(Activator.CreateInstance(player), null));
            }
            finally { context.Unload(); }
        }
        finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void FailedRecompileRemovesPreviousAssemblyAndManifest()
    {
        string root = Path.Combine(Path.GetTempPath(), "aether-compile-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            string source = Path.Combine(root, "Player.cs");
            string output = Path.Combine(root, "bin", "AetherGame.dll");
            Type compiler = Assembly.Load("AetherCore.Interop").GetType("AetherCore.Interop.ScriptCompiler", true)!;
            MethodInfo compile = compiler.GetMethod("Compile", BindingFlags.Static | BindingFlags.NonPublic)!;
            object?[] args = [root, output, Path.GetDirectoryName(typeof(EntityScript).Assembly.Location)!, true, null];
            File.WriteAllText(source, "public sealed class Player : AetherCore.EntityScript { }");
            Assert.True((int)compile.Invoke(null, args)! == 0, args[4]?.ToString());
            Assert.True(File.Exists(Path.ChangeExtension(output, ".deps.json")));
            File.WriteAllText(source, "this is not valid C#");
            Assert.NotEqual(0, (int)compile.Invoke(null, args)!);
            Assert.False(File.Exists(output));
            Assert.False(File.Exists(Path.ChangeExtension(output, ".deps.json")));
        }
        finally { Directory.Delete(root, true); }
    }
}

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Emit;
using Microsoft.CodeAnalysis.Text;

namespace AetherCore.Interop;

/// <summary>
/// Compiles a project's C# scripts in-process, with Roslyn.
/// </summary>
/// <remarks>
/// <para>
/// This replaces shelling out to <c>dotnet build</c>, and the reason is a bug report: someone
/// without Visual Studio downloaded the editor, pressed Play, and was told their script failed
/// to compile. Nothing was wrong with the script. <c>dotnet.exe</c> ships with the .NET
/// RUNTIME, so it exists on machines that have no compiler at all, and MSBuild's own "a
/// compatible .NET SDK was not found" surfaced as if the project were broken.
/// </para>
/// <para>
/// Detecting that better only improves the wording. The actual fix is not to need an SDK: the editor
/// already hosts the .NET runtime, so it can host the compiler too. That is how Unity does it,
/// and it is what makes "download one file and make a game" true rather than nearly true.
/// </para>
/// <para>
/// References come from the assemblies this process is ALREADY running on, read out of
/// TRUSTED_PLATFORM_ASSEMBLIES, plus whatever sits beside the engine's own managed output.
/// That is deliberate: it needs no reference pack shipped alongside, and it guarantees scripts
/// are compiled against exactly the runtime that will execute them, which a separately
/// versioned ref pack cannot promise.
/// </para>
/// <para>
/// What is given up is MSBuild: NuGet PackageReferences, analyzers and multi-project builds do
/// not apply here. A generated game project has none of those - it is one directory of .cs
/// files against the SDK assembly - and the .csproj is still written out so an IDE can offer
/// IntelliSense over the same sources.
/// </para>
/// </remarks>
internal static class ScriptCompiler
{
    // Directories that hold build output rather than source. Compiling these would feed a
    // previous build's generated files back into the next one.
    private static readonly string[] ExcludedDirectories = ["obj", "bin", "artifacts", "Builds", ".vs", ".git"];

    internal static int Compile(string scriptDir, string outputPath, string referenceDir, bool optimize, out string diagnostics)
    {
        diagnostics = string.Empty;
        try
        {
            if (!Directory.Exists(scriptDir))
            {
                diagnostics = $"Script directory not found: {scriptDir}";
                return 1;
            }

            List<SyntaxTree> trees = [];
            CSharpParseOptions parseOptions = new(LanguageVersion.Latest);
            foreach (string file in EnumerateSources(scriptDir))
            {
                // Explicit UTF-8 with a path attached: the path is what a diagnostic quotes
                // back, so it has to be the real one rather than a temporary.
                string text = File.ReadAllText(file);
                trees.Add(CSharpSyntaxTree.ParseText(SourceText.From(text, Encoding.UTF8), parseOptions, file));
            }

            if (trees.Count == 0)
            {
                // Not an error. A project with no scripts yet still needs an assembly to
                // exist, or every load after this reports a missing file.
                trees.Add(CSharpSyntaxTree.ParseText("", parseOptions));
            }

            CSharpCompilation compilation = CSharpCompilation.Create(
                assemblyName: "AetherGame",
                syntaxTrees: trees,
                references: CollectReferences(referenceDir),
                options: new CSharpCompilationOptions(
                    OutputKind.DynamicallyLinkedLibrary,
                    optimizationLevel: optimize ? OptimizationLevel.Release : OptimizationLevel.Debug,
                    allowUnsafe: true,
                    nullableContextOptions: NullableContextOptions.Enable));

            Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
            string pdbPath = Path.ChangeExtension(outputPath, ".pdb");

            // Portable PDBs, and emitted to their own file rather than embedded: the editor
            // deploys the .pdb beside the assembly so a debugger can attach to running
            // scripts, which is the same thing the old MSBuild invocation asked for.
            EmitResult result;
            if (optimize)
            {
                // A shipped build carries no symbols, matching what the publish step asked
                // MSBuild for before this.
                using FileStream peOnly = File.Create(outputPath);
                result = compilation.Emit(peOnly);
            }
            else
            {
                using FileStream peStream = File.Create(outputPath);
                using FileStream pdbStream = File.Create(pdbPath);
                result = compilation.Emit(peStream, pdbStream,
                    options: new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb));
            }

            if (!result.Success)
            {
                diagnostics = Format(result.Diagnostics);
                // A failed emit leaves a truncated assembly behind, which would then load as
                // if it were the build's real output.
                TryDelete(outputPath);
                TryDelete(pdbPath);
                return 2;
            }

            // Warnings are worth surfacing even when the build worked.
            string warnings = Format(result.Diagnostics.Where(d => d.Severity == DiagnosticSeverity.Warning));
            if (warnings.Length > 0)
            {
                diagnostics = warnings;
            }
            return 0;
        }
        catch (Exception ex)
        {
            diagnostics = ex.ToString();
            return 3;
        }
    }

    private static IEnumerable<string> EnumerateSources(string scriptDir)
    {
        foreach (string file in Directory.EnumerateFiles(scriptDir, "*.cs", SearchOption.AllDirectories))
        {
            string relative = Path.GetRelativePath(scriptDir, file);
            string[] segments = relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            bool excluded = segments.Any(segment =>
                ExcludedDirectories.Contains(segment, StringComparer.OrdinalIgnoreCase));
            if (!excluded)
            {
                yield return file;
            }
        }
    }

    private static List<MetadataReference> CollectReferences(string referenceDir)
    {
        Dictionary<string, MetadataReference> byName = new(StringComparer.OrdinalIgnoreCase);

        // Everything this process was started with: the whole framework, at the exact
        // version that will run the result.
        if (AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") is string trusted)
        {
            foreach (string path in trusted.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
            {
                AddReference(byName, path);
            }
        }

        // The engine SDK assembly and anything shipped beside it. Added second so a project
        // sees the copy the editor actually loaded rather than one the runtime happened to
        // carry under the same name.
        if (!string.IsNullOrEmpty(referenceDir) && Directory.Exists(referenceDir))
        {
            foreach (string path in Directory.EnumerateFiles(referenceDir, "*.dll"))
            {
                // The game's own previous output must never be a reference to itself.
                if (!Path.GetFileName(path).Equals("AetherGame.dll", StringComparison.OrdinalIgnoreCase))
                {
                    AddReference(byName, path, replace: true);
                }
            }
        }

        return [.. byName.Values];
    }

    private static void AddReference(Dictionary<string, MetadataReference> byName, string path, bool replace = false)
    {
        string key = Path.GetFileNameWithoutExtension(path);
        if (!replace && byName.ContainsKey(key))
        {
            return;
        }
        try
        {
            byName[key] = MetadataReference.CreateFromFile(path);
        }
        catch
        {
            // A native image or an unreadable file in the same folder is not a reference;
            // skipping it is correct and quieter than failing the whole compile.
        }
    }

    private static string Format(IEnumerable<Diagnostic> diagnostics)
    {
        IEnumerable<Diagnostic> interesting = diagnostics
            .Where(d => d.Severity is DiagnosticSeverity.Error or DiagnosticSeverity.Warning)
            .OrderByDescending(d => d.Severity);

        StringBuilder sb = new();
        foreach (Diagnostic d in interesting.Take(50))
        {
            // Same shape MSBuild prints, so existing habits and any log scraping still read.
            FileLinePositionSpan span = d.Location.GetLineSpan();
            if (!string.IsNullOrEmpty(span.Path))
            {
                sb.Append(span.Path)
                  .Append('(').Append(span.StartLinePosition.Line + 1)
                  .Append(',').Append(span.StartLinePosition.Character + 1).Append("): ");
            }
            sb.Append(d.Severity == DiagnosticSeverity.Error ? "error " : "warning ")
              .Append(d.Id).Append(": ").AppendLine(d.GetMessage());
        }
        return sb.ToString();
    }

    private static void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch
        {
            // Best effort; the caller already treats this build as failed.
        }
    }
}

using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// ponytail: dev-only look-dev hook (logs/look/FxLook/README.md), in the style of the debug warp. While
/// project://fx-&lt;AETHER_CONTROL_PORT&gt;.txt exists it is read every frame; each new content
/// "&lt;seq&gt; &lt;effect&gt; x y z" fires that effect's visuals once at (x, y, z) - no sound, damage or
/// gameplay - so a paused editor can step a blast frame by frame for before/after strips.
/// Inert in normal play (no port, no file). Remove when automated testing gets a driver API.
/// </summary>
public static class FxPreview
{
	private static readonly string? kFile = Environment.GetEnvironmentVariable("AETHER_CONTROL_PORT") is { Length: > 0 } port
		? $"project://fx-{port}.txt" : null;
	private static string? s_last;

	public static void Poll()
	{
		if (kFile == null || Assets.ReadText(kFile) is not string text || text == s_last)
		{
			return;
		}
		s_last = text;
		string[] p = text.Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
		if (p.Length < 5 || !float.TryParse(p[2], out float x) || !float.TryParse(p[3], out float y) || !float.TryParse(p[4], out float z))
		{
			return;
		}
		Vector3 at = new(x, y, z);
		switch (p[1])
		{
			case "nitro": CrateFx.PreviewExplosion(at, 4); break;
			case "tnt": CrateFx.PreviewExplosion(at, 3); break;
			case "legacy": CrateFx.ForceDisc = x != 0.0f; break;
			case "blast": CrateFx.PreviewModernBlast(at, false); break;
			case "nblast": CrateFx.PreviewModernBlast(at, true); break;
			case "bomb": CrateFx.PreviewBomb(at); break;
			case "muzzle": CrateFx.MuzzleFlash(at); break;
			case "break": CrateFx.PreviewBreak(at); break;
			case "impact": CrateFx.ImpactFlash(at); break;
			case "gem": CrateFx.GemPickup(at); break;
			case "wumpa": CrateFx.WumpaPickup(at); break;
			case "pop": CrateFx.CreaturePop(at, false); break;
			case "feathers": CrateFx.CreaturePop(at, true); break;
			case "slam": CrateFx.PreviewLanding(at, true); break;
			case "spin": CrateFx.SpinSwirlAt(at, 0.4f, true); break;
			case "land": CrateFx.PreviewLanding(at, false); break;
			default: return;
		}
		Log.Info($"[Twinsanity] fx preview {p[1]} at ({x}, {y}, {z})");
	}
}

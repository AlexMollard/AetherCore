using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// One keeper, as everybody else sees them: a lantern standing in the dark, and the four
/// numbers the congregation panel reads.
/// </summary>
/// <remarks>
/// <para>
/// Attached to the replicated <c>vigil</c> prefab, so every peer runs one instance per
/// keeper in the session and exactly one of them - the locally owned copy - has anything
/// to say. The owner writes its own numbers each frame and replication carries them; the
/// copies on other machines never compute them, which is the point.
/// </para>
/// <para>
/// <b>Why the rate travels as a base-10 logarithm.</b> Replicated fields are float, int,
/// bool, Vector3, string or enum - and an idle game's production passes 1e38 in a long
/// run, which a float cannot hold at all. The log fits in a float with room to spare
/// forever, costs one <c>Pow</c> to read back, and is exact enough for a number that is
/// only ever shown to three significant figures.
/// </para>
/// </remarks>
public sealed class VigilPresence : EntityScript
{
	/// <summary>Log10 of this keeper's ichor per second. Well below any real rate when idle,
	/// so a keeper who has lit nothing reads as zero rather than as one.</summary>
	[Replicated] public float RateLog10 = -30.0f;

	/// <summary>Their dread, 0 to 1. The reason to look at someone else's lantern.</summary>
	[Replicated] public float Dread;

	[Replicated] public int Sigils;

	/// <summary>Deepest rite they hold, or -1. Sets their lantern's colour, so the parish
	/// shows at a glance who has gone furthest down.</summary>
	[Replicated] public int Deepest = -1;

	/// <summary>Every presence this peer is holding, the local one included. Rebuilt by
	/// attach/detach rather than scanned per frame.</summary>
	public static readonly List<VigilPresence> All = new List<VigilPresence>();

	/// <summary>The locally owned keeper, or null in a session this peer has not spawned
	/// into yet.</summary>
	public static VigilPresence? Local;

	private float _nameTimer;

	public string Keeper
	{
		get
		{
			string name = Net.GetPlayerName(Self);
			return string.IsNullOrEmpty(name) ? "Keeper" : name;
		}
	}

	public uint Connection => Net.OwnerOf(Self);
	public bool IsLocal => Net.HasAuthority(Self);

	/// <summary>Their production, reconstructed from the replicated logarithm.</summary>
	public double Rate => RateLog10 <= -29.0f ? 0.0 : Math.Pow(10.0, RateLog10);

	public override void OnAttach()
	{
		All.Add(this);
		if (IsLocal)
		{
			Local = this;
			// Claim the name the keeper typed at the threshold. The engine hands back what it
			// actually granted - a duplicate is disambiguated rather than refused - so the
			// local vigil adopts the granted name instead of the requested one.
			Vigil.KeeperName = Net.ClaimPlayerName(Self, Vigil.KeeperName);
		}
	}

	public override void OnDetach()
	{
		All.Remove(this);
		if (ReferenceEquals(Local, this))
		{
			Local = null;
		}
	}

	public override void OnUpdate(float deltaTime)
	{
		if (IsLocal)
		{
			Publish(deltaTime);
		}
		Present();
	}

	/// <summary>Copy the local simulation into the replicated fields.</summary>
	private void Publish(float deltaTime)
	{
		double rate = Vigil.Rate;
		RateLog10 = rate > 0.0 ? (float)Math.Log10(rate) : -30.0f;
		Dread = (float)Vigil.Dread;
		Sigils = Vigil.Sigils;
		Deepest = Vigil.DeepestRite();

		// A rename is rare and the claim is a round trip, so it is re-asserted on a slow
		// timer rather than every frame - enough to recover from a name that lost a race at
		// join time, cheap enough to leave running.
		_nameTimer -= deltaTime;
		if (_nameTimer <= 0.0f)
		{
			_nameTimer = 5.0f;
			if (Net.GetPlayerName(Self) != Vigil.KeeperName)
			{
				Vigil.KeeperName = Net.ClaimPlayerName(Self, Vigil.KeeperName);
			}
		}
	}

	/// <summary>Drive this keeper's lantern from whatever the fields currently hold. Runs on
	/// every peer for every keeper, the local one included, so one code path decides how a
	/// lantern looks and no copy can disagree with another.</summary>
	private void Present()
	{
		Vector4 tint = Deepest >= 0 && Deepest < Content.RiteCount
			? Content.Rites[Deepest].Colour
			: Palette.TextDim;
		// Dread pulls the flame toward rust. A keeper about to be visited is visibly the
		// wrong colour from across the parish.
		Vector4 lit = Palette.Mix(tint, Palette.Dread, Math.Clamp(Dread, 0.0f, 1.0f) * 0.85f);

		ComponentAccess sprite = Self.Component("Sprite Renderer");
		if (sprite.Exists)
		{
			sprite.SetVector4("tint", lit);
		}

		ComponentAccess light = Self.Component("Point Light");
		if (light.Exists)
		{
			light.SetVector3("color", new Vector3(lit.X, lit.Y, lit.Z));
			// Brightness follows the logarithm directly: a keeper producing a thousand times
			// more should read as brighter, not as a hundred times off the top of the scale.
			float brightness = Math.Clamp((RateLog10 + 2.0f) / 8.0f, 0.0f, 1.0f);
			light.SetFloat("intensity", 1.2f + brightness * 4.0f);
			light.SetFloat("flicker", 0.15f + Math.Clamp(Dread, 0.0f, 1.0f) * 0.7f);
			light.SetFloat("flickerSpeed", 4.0f + Math.Clamp(Dread, 0.0f, 1.0f) * 14.0f);
		}
	}

	/// <summary>Find the presence owned by a connection, or null. Used to address a tithe or
	/// a shunt at somebody.</summary>
	public static VigilPresence? ForConnection(uint connection)
	{
		for (int i = 0; i < All.Count; i++)
		{
			if (All[i].Connection == connection)
			{
				return All[i];
			}
		}
		return null;
	}
}

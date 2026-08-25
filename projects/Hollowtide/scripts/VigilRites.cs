using System;
using System.Collections.Generic;
using System.Globalization;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Everything one keeper can do TO another: ring the bell, tithe ichor across, and shunt
/// dread onto somebody who was not expecting it.
/// </summary>
/// <remarks>
/// <para>
/// <b>Why this lives on the vigil prefab.</b> The host only accepts a
/// <see cref="NetRpcTarget.Server"/> call aimed at an entity the SENDING connection owns,
/// so a request has to be declared on a script attached to the caller's own keeper.
/// Putting it on a scene-placed, host-owned entity would compile, route, and then be
/// dropped on arrival.
/// </para>
/// <para>
/// <b>Why every payload is one string.</b> The RPC channel marshals at most a single
/// string argument, so each message is a pipe-separated record parsed at both ends. One
/// encoder and one decoder, right next to each other, is the version of that constraint
/// least likely to drift - the alternative is a family of near-identical RPC methods that
/// each have to be added on both sides.
/// </para>
/// <para>
/// A client is trusted for the SIZE of what it gives away, because only that client's
/// process knows its own purse - the host replicates a rate, not a balance. That is a fine
/// trade for a co-operative vigil: the worst a liar can do here is be generous.
/// </para>
/// </remarks>
public sealed class VigilRites : EntityScript
{
	/// <summary>Seconds two bells must fall within to answer each other.</summary>
	private const double kCommunionWindow = 6.0;

	/// <summary>Surge granted to a keeper who rings alone, and to the whole congregation when
	/// the bell is answered. The lone pair is the vigil's, because the client grants exactly the
	/// same bell to itself when its keeper entity has not spawned yet, and two spellings of one
	/// bell is two bells.</summary>
	private const double kLoneSeconds = Vigil.kLoneBellSeconds;
	private const double kLoneMultiplier = Vigil.kLoneBellMultiplier;
	private const double kCommunionSeconds = Vigil.kAnsweredBellSeconds;
	private const double kCommunionMultiplier = Vigil.kAnsweredBellMultiplier;

	// Host-only bookkeeping: who rang, and when. Static because the host holds one instance
	// of this script per keeper and the window is a property of the SESSION, not of a keeper.
	private static uint s_lastRinger = uint.MaxValue;
	private static double s_lastRingAt = -1000.0;

	/// <summary>
	/// When each connection last got a bell out of the host.
	/// </summary>
	/// <remarks>
	/// <para>
	/// The interval between bells was enforced by the button being greyed out, and by nothing
	/// else. Everywhere else in this file a client is trusted only for the size of what it gives
	/// AWAY - the note above says the worst a liar can do is be generous - but the bell is not a
	/// gift. It multiplies the whole congregation's economy for a third of its cooldown, and a
	/// client that simply sent the message on a loop would have held every keeper in the session
	/// at a permanent surge. That is not generosity, and it is not something the other players
	/// opted into.
	/// </para>
	/// <para>
	/// A shade under the real interval, because the client is running its own timer against its
	/// own clock and a ring that arrives a few milliseconds early is a legitimate press, not an
	/// attack. Anything sooner than that was not pressed by a person.
	/// </para>
	/// </remarks>
	private static readonly Dictionary<uint, double> s_ringingSince = new();

	/// <summary>How much early a ring may arrive and still be a real one.</summary>
	private const double kRingSlack = 1.0;

	/// <summary>
	/// When each connection last turned something loose on somebody.
	/// </summary>
	/// <remarks>
	/// The note at the top of this file says a client is trusted for the size of what it gives
	/// AWAY, on the reasoning that the worst a liar can do is be generous. That reasoning holds
	/// for a tithe and for a relic. It does not hold here, because the shunt is the one message
	/// that acts on somebody against their interest: it raises another keeper's dread, and dread
	/// is what brings a visitation. Unlimited and unclamped, one client could hold every other
	/// keeper in the session pinned at the brink - which is not generosity, and not something
	/// they agreed to by joining.
	/// </remarks>
	private static readonly Dictionary<uint, double> s_shuntingSince = new();

	// ── Sending ──────────────────────────────────────────────────────────────────────

	/// <summary>Ring the bell. Everything after this is the host's decision.</summary>
	public void RingBell() => Send("bell");

	/// <summary>Give ichor away. Deducted here, because this process is the only one that
	/// knows the purse; nothing is sent if the deduction fails.</summary>
	public bool Tithe(uint toConnection, double amount)
	{
		if (!Vigil.SpendForTithe(amount))
		{
			return false;
		}
		return Send("tithe|" + toConnection + "|" + amount.ToString("R", CultureInfo.InvariantCulture));
	}

	/// <summary>Push a quarter of your dread onto someone else. It leaves you whether or not
	/// they are still there to receive it - which is the deal.</summary>
	public bool Shunt(uint toConnection)
	{
		double shed = Vigil.ShuntToKeeper();
		if (shed <= 0.0)
		{
			return false;
		}
		return Send("shunt|" + toConnection + "|" + shed.ToString("R", CultureInfo.InvariantCulture));
	}

	/// <summary>
	/// Hand a relic to another keeper.
	/// </summary>
	/// <remarks>
	/// A relic travels as its seed and its grade, and nothing else - the name, the powers and
	/// the drawing are all regenerated on the far side from those two numbers. That is what
	/// makes trading cheap enough to be worth having: the same seed is the same relic in
	/// anyone's hands, so there is nothing to serialise and nothing to keep in step.
	/// </remarks>
	public bool GiveRelic(uint toConnection, int satchelIndex)
	{
		if (!Vigil.GiveRelic(satchelIndex, out Relic given))
		{
			return false;
		}
		if (Send("relic|" + toConnection + "|" + given.Seed + "|" + (int)given.Grade))
		{
			return true;
		}
		// The send failed after the satchel was already lightened - put it back rather than
		// letting a dropped packet eat the relic.
		Vigil.RestoreRelic(given);
		return false;
	}

	private bool Send(string payload)
	{
		if (!Net.IsConnected)
		{
			return false;
		}
		if (!Net.Call(Self, nameof(Petition), payload))
		{
			// The only ordinary way this refuses is an entity with no net id yet: a button
			// pressed in the handful of frames between joining and the spawn round trip
			// finishing. Worth saying rather than losing silently.
			Log.Warn("[Hollowtide] rite dropped, this keeper is not replicated yet");
			return false;
		}
		return true;
	}

	// ── Host ─────────────────────────────────────────────────────────────────────────

	/// <summary>Client to host: a request. The host decides what it means and tells everyone.</summary>
	[NetRpc(NetRpcTarget.Server)]
	public void Petition(string payload)
	{
		string[] parts = payload.Split('|');
		if (parts.Length == 0)
		{
			return;
		}
		// The sender's name comes from the host's own replicated copy rather than from the
		// message, so attribution is never something the sender chose.
		string from = Net.GetPlayerName(Self);
		if (string.IsNullOrEmpty(from))
		{
			from = "A keeper";
		}
		uint sender = Net.OwnerOf(Self);

		switch (parts[0])
		{
			case "bell":
				Bell(sender, from);
				break;

			case "tithe" when parts.Length >= 3:
				if (uint.TryParse(parts[1], out uint tithed) && ParseAmount(parts[2], out double gift))
				{
					Proclaim("tithe|" + tithed + "|" + gift.ToString("R", CultureInfo.InvariantCulture) + "|" + from);
				}
				break;

			case "shunt" when parts.Length >= 3:
				if (uint.TryParse(parts[1], out uint shunted) && ParseAmount(parts[2], out double dread))
				{
					double since = s_shuntingSince.TryGetValue(sender, out double last)
						? Time.UnscaledTime - last
						: double.MaxValue;
					if (since < Vigil.kShuntInterval - kRingSlack)
					{
						break;
					}
					s_shuntingSince[sender] = Time.UnscaledTime;
					// Clamped to what a shunt IS. The amount is the sender's own claim about its
					// own meter, which is the right shape - only that process knows what it is
					// carrying - but nothing here has to believe a claim larger than the verb.
					dread = Math.Min(dread, Vigil.kShuntShare);
					Proclaim("shunt|" + shunted + "|" + dread.ToString("R", CultureInfo.InvariantCulture) + "|" + from);
				}
				break;

			case "relic" when parts.Length >= 4:
				if (uint.TryParse(parts[1], out uint toKeeper)
					&& int.TryParse(parts[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out int seed)
					&& int.TryParse(parts[3], NumberStyles.Integer, CultureInfo.InvariantCulture, out int grade))
				{
					Proclaim("relic|" + toKeeper + "|" + seed + "|" + grade + "|" + from);
				}
				break;
		}
	}

	/// <summary>The whole point of ringing: one bell is worth something on its own, and a
	/// second bell from a DIFFERENT keeper inside the window turns both into a communion
	/// that pays the entire congregation.</summary>
	private void Bell(uint sender, string from)
	{
		double now = Time.UnscaledTime;
		if (s_ringingSince.TryGetValue(sender, out double rang) && now - rang < Vigil.kBellInterval - kRingSlack)
		{
			// Dropped in silence. There is nothing useful to tell the sender: an honest client
			// cannot get here, and a dishonest one is not owed an explanation of the limit.
			return;
		}
		s_ringingSince[sender] = now;

		bool answered = s_lastRinger != uint.MaxValue
			&& s_lastRinger != sender
			&& now - s_lastRingAt <= kCommunionWindow;

		if (answered)
		{
			s_lastRinger = uint.MaxValue;
			s_lastRingAt = -1000.0;
			Proclaim("surge|all|" + kCommunionSeconds.ToString(CultureInfo.InvariantCulture) + "|" +
				kCommunionMultiplier.ToString(CultureInfo.InvariantCulture) + "|" +
				from + " answers the bell. Everything in the parish leans in.");
			return;
		}

		s_lastRinger = sender;
		s_lastRingAt = now;
		Proclaim("surge|" + sender + "|" + kLoneSeconds.ToString(CultureInfo.InvariantCulture) + "|" +
			kLoneMultiplier.ToString(CultureInfo.InvariantCulture) + "|" +
			from + " rings the bell. It goes out into the dark unanswered.");
	}

	private void Proclaim(string payload) => Net.Call(Self, nameof(Receive), payload);

	// ── Everyone ─────────────────────────────────────────────────────────────────────

	/// <summary>Host to every peer, the host included. Runs on the SENDER's keeper entity on
	/// each machine, so the only way to know whether a message is for this player is to
	/// compare the connection it names against this peer's own.</summary>
	[NetRpc(NetRpcTarget.Multicast)]
	public void Receive(string payload)
	{
		string[] parts = payload.Split('|');
		if (parts.Length == 0)
		{
			return;
		}
		uint me = Net.LocalConnectionId;

		switch (parts[0])
		{
			case "surge" when parts.Length >= 5:
			{
				bool everyone = parts[1] == "all";
				bool mine = everyone || (uint.TryParse(parts[1], out uint who) && who == me);
				if (mine
					&& double.TryParse(parts[2], NumberStyles.Float, CultureInfo.InvariantCulture, out double seconds)
					&& double.TryParse(parts[3], NumberStyles.Float, CultureInfo.InvariantCulture, out double mult))
				{
					Vigil.BeginSurge(seconds, mult, everyone);
				}
				Vigil.Announce?.Invoke(parts[4], everyone ? Omen.Good : Omen.Plain);
				break;
			}

			case "tithe" when parts.Length >= 4:
				if (uint.TryParse(parts[1], out uint toGift) && ParseAmount(parts[2], out double gift))
				{
					if (toGift == me)
					{
						Vigil.ReceiveTithe(gift, parts[3]);
					}
					else
					{
						Vigil.Announce?.Invoke(parts[3] + " tithes " + Numbers.Short(gift) + " to another keeper.", Omen.Plain);
					}
				}
				break;

			case "shunt" when parts.Length >= 4:
				if (uint.TryParse(parts[1], out uint toDread) && ParseAmount(parts[2], out double dread))
				{
					if (toDread == me)
					{
						Vigil.ReceiveDread(dread, parts[3]);
					}
					else
					{
						Vigil.Announce?.Invoke(parts[3] + " turns something loose on another keeper.", Omen.Dread);
					}
				}
				break;

			case "relic" when parts.Length >= 5:
				if (uint.TryParse(parts[1], out uint toRelic)
					&& int.TryParse(parts[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out int seed)
					&& int.TryParse(parts[3], NumberStyles.Integer, CultureInfo.InvariantCulture, out int grade))
				{
					if (toRelic == me)
					{
						Vigil.ReceiveRelic(seed, grade, parts[4]);
					}
					else
					{
						Vigil.Announce?.Invoke(parts[4] + " hands something to another keeper.", Omen.Plain);
					}
				}
				break;

			case "say" when parts.Length >= 2:
				Vigil.Announce?.Invoke(parts[1], Omen.Plain);
				break;
		}
	}

	private static bool ParseAmount(string text, out double value)
	{
		bool ok = double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out value);
		// A non-finite or negative amount is a malformed message, not a gift of infinity.
		if (!ok || double.IsNaN(value) || double.IsInfinity(value) || value < 0.0)
		{
			value = 0.0;
			return false;
		}
		return true;
	}
}

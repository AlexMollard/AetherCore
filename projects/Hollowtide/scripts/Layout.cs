using System;
using System.Numerics;

namespace AetherGame;

/// <summary>
/// Where things stand in the parish, as pure arithmetic.
/// </summary>
/// <remarks>
/// <para>
/// Split out of <c>Parish</c> for one reason: none of it needs the engine, and everything in
/// here was a claim that could only be checked by looking at the screen. The conduits are the
/// case that forced it - the assertion that a wire from the leftmost rite to the sigil never
/// crosses anything is a statement about a cubic and a clearance, provable without drawing
/// anything, and it was first "proved" by working one example on paper. That example was wrong:
/// the route it justified ran straight across the stoke and ward buttons at the window the game
/// was actually being played at. The harness can hold the claim instead, at every parish size
/// and every window shape, which is what the rest of this game's rules already get.
/// </para>
/// <para>
/// Engine-free by design, exactly like <c>Vigil</c>, <c>Relics</c> and <c>Palette</c>: no
/// <c>using AetherCore</c>, so it compiles standalone into <c>tools/balance</c>.
/// <c>System.Numerics.Vector2</c> is the base library, not the engine. Everything here takes
/// live measurements as plain fractions and hands back fractions - reading rects stays in
/// <c>Parish</c>, where it belongs.
/// </para>
/// <para>
/// Every figure is a fraction of the BACKDROP, which covers the whole canvas, with y increasing
/// DOWNWARD. So a smaller y is higher up, and the arch below has a y BELOW the horizon's.
/// Fractions rather than pixels so the parish reflows with the window instead of baking a
/// resolution.
/// </para>
/// </remarks>
public static class Layout
{
	/// <summary>Height of the horizon, matching the same constant in ui_parish.slang. The rites
	/// stand on it, so the two MUST agree.</summary>
	public const float Horizon = 0.62f;

	/// <summary>Left end of the stretch of horizon the rites stand along.</summary>
	public const float RiteLeft = 0.035f;

	/// <summary>Margin between the sigil's resting edge and the deepest rite.</summary>
	public const float SigilMargin = 0.03f;

	/// <summary>How far LEFT of the sigil's centre the conduits arrive, as a fraction of its
	/// resting width. Off-centre so the run of wire is visibly to one side of the mark rather
	/// than balanced on top of it.</summary>
	public const float IntakeLeft = 0.18f;

	/// <summary>How far ABOVE the sigil's centre they arrive, as a fraction of its resting
	/// height. Just inside the rim, so a line meets stone rather than stopping in the air.</summary>
	public const float IntakeRise = 0.44f;

	/// <summary>The most purchases one rolling front can stand for. Three bits of the packed
	/// slot, so eight.</summary>
	public const int WaveWeight = 8;

	/// <summary>
	/// One rolling front's origin, weight and progress squeezed into a single float in [0,1).
	/// </summary>
	/// <remarks>
	/// <para>
	/// The high part carries the origin in 32 steps and the weight in 8, as origin * 8 + weight;
	/// the low part carries the progress. Kept inside [0,1) rather than spread over a wider range
	/// because these ride in colour alphas, and a colour channel is not a place to assume nothing
	/// will ever clamp. Zero means idle, which a live front cannot collide with: a weight is
	/// never below 1, so a live slot's high part is never 0.
	/// </para>
	/// <para>
	/// The origin used to have all 255 steps to itself. Giving three of them to the weight leaves
	/// it 32, which is a thirtieth of the width - and the only thing that reads it is a falloff
	/// four fifths of the screen wide, so the quantisation is far below anything visible.
	/// Spending real precision on a value nothing needs precisely is how the pool came to have a
	/// hard ceiling in the first place.
	/// </para>
	/// <para>
	/// Here rather than in <c>Parish</c> because it is arithmetic shared with a shader, and a
	/// packing bug is invisible until someone notices a front in the wrong place. Unpacked by
	/// <c>Unwave</c> in <c>ui_parish.slang</c> - one format in THREE places now, so the harness
	/// round-trips it.
	/// </para>
	/// </remarks>
	public static float PackWave(float origin, int weight, float progress)
	{
		if (progress <= 0.0f)
		{
			return 0.0f;
		}
		float steps = MathF.Floor(Math.Clamp(origin, 0.0f, 1.0f) * 31.0f);
		float held = Math.Clamp(weight, 1, WaveWeight) - 1;
		return (steps * 8.0f + held + MathF.Min(progress, 0.999f)) / 256.0f;
	}

	/// <summary>
	/// Undo <see cref="PackWave"/>.
	/// </summary>
	/// <remarks>
	/// A hand-copy of <c>Unwave</c> in <c>ui_parish.slang</c>, and nothing but the harness calls
	/// it. Round-tripping a packed front through this proves the C# side is self-consistent -
	/// which is worth having, and is NOT the same as proving the shader agrees. That claim used
	/// to be made here, and it is precisely the mistake that let the relic power mask ship
	/// wrong: a check that compares one side of a boundary against a copy of itself passes
	/// however far the other side has drifted. What actually holds the shader is
	/// <c>TheShadersAgreeWithTheGame</c>, which reads the file.
	/// </remarks>
	public static (float Origin, float Progress, int Weight) UnpackWave(float packed)
	{
		float scaled = Math.Clamp(packed, 0.0f, 1.0f) * 256.0f;
		float high = MathF.Floor(scaled);
		int weight = (int)MathF.Floor(high % 8.0f) + 1;
		float origin = MathF.Floor(high / 8.0f) / 31.0f;
		return (origin, scaled - high, weight);
	}

	/// <summary>
	/// How far above the tallest thing in the parish the conduits arch.
	/// </summary>
	/// <remarks>
	/// The arch goes OVER the parish rather than under it, and the clearance is measured against
	/// whatever is actually standing rather than written down, because a rite's height is a log
	/// curve on how many the keeper owns and is capped by the spacing - so the tallest thing on
	/// screen depends on both the parish and the window.
	/// </remarks>
	public const float Clearance = 0.035f;

	/// <summary>Nearest the top edge the arch is ever allowed. On a short, wide window the parish
	/// can grow tall enough that clearing it would take the arch off screen; the arch flattens
	/// against this instead, which grazes the tallest crown rather than vanishing.</summary>
	public const float Ceiling = 0.03f;

	/// <summary>
	/// Room left above a rite for the count that sits there, in pixels.
	/// </summary>
	/// <remarks>
	/// Derived from where Parish actually puts that count - eight above the crown, in a box of
	/// its own scaled height - rather than written down. It was a flat 30, and when the type was
	/// made larger the count grew past it, which would have laid the canopy across the very
	/// numbers it exists to clear.
	/// <para>
	/// Deriving it keeps that clearance right at any type scale, but do not mistake it for a
	/// checked one. The parish's clearance invariants pass at ANY scale, including absurd ones,
	/// and this was tested rather than assumed: at six times the type they all still hold. They
	/// have to - RiteHeightCap subtracts this allowance, so a larger count simply makes the
	/// structures shorter and the geometry stays consistent with itself the whole way down. What
	/// no check here can see is the type outgrowing its BOXES, which is what actually breaks
	/// first. That one still needs eyes.
	/// </para>
	/// </remarks>
	public static readonly float CountAllowance = 8.0f + Typography.Box(21.0f);

	/// <summary>
	/// The tallest a rite may be drawn, as a fraction of the window height.
	/// </summary>
	/// <remarks>
	/// There is only <see cref="Horizon"/> of the window above the line the rites stand on, and
	/// three things have to fit inside it: the structure, the count above it, and the canopy the
	/// conduits arch along above that. The cap used to be a flat 0.60 of the window, which is
	/// more than the 0.62 available even before the count - so on a wide, short window a heavily
	/// bought rite drew its count off the top of the screen, and the canopy, having nowhere to go,
	/// clamped BELOW the crowns and sent every conduit through the structures it was meant to
	/// clear. Derived from the constants it has to live with rather than picked, so the three
	/// cannot drift apart again.
	/// </remarks>
	public static float RiteHeightCap(float windowHeight)
		=> MathF.Max(0.05f, Horizon - Ceiling - Clearance - CountAllowance / MathF.Max(windowHeight, 1.0f));

	/// <summary>The widest a rite can be, as a fraction of the window WIDTH. Rites are drawn
	/// square, so this is the height cap turned sideways.</summary>
	public static float RiteWidthCap(float windowWidth, float windowHeight)
		=> RiteHeightCap(windowHeight) * windowHeight / MathF.Max(windowWidth, 1.0f);

	/// <summary>Where a rite's conduit leaves it: above its crown, clear of its count.</summary>
	public static float CrownY(float heightFraction, float windowHeight)
		=> Horizon - heightFraction - CountAllowance / MathF.Max(windowHeight, 1.0f);

	/// <summary>
	/// Right end of the stretch of horizon the rites stand along: just clear of the sigil.
	/// </summary>
	/// <remarks>
	/// Measured off the sigil rather than written down as a constant. The sigil is centred in the
	/// nave and the nave is not centred in the canvas, so any number picked by hand is only
	/// correct at one window size - and the one picked by hand put the deepest rite on top of it.
	/// <para>
	/// Takes the sigil's CENTRE and its RESTING width, never its live edge. The sigil is
	/// centre-pivoted and grows about a tenth when struck, so its left edge slides outward on
	/// every click - and this figure sets the spacing and the position of every structure in the
	/// parish. Reading the edge meant the whole parish shuffled sideways each time the keeper
	/// gathered. The centre does not move when the sigil scales.
	/// </para>
	/// <para>
	/// Clamped at BOTH ends. The upper bound is the load-bearing one: callers divide live rects
	/// to get <paramref name="restWidth"/>, and an unbounded result here propagates straight into
	/// the size of every rite on screen.
	/// </para>
	/// </remarks>
	/// <param name="centreX">The sigil's centre, as a fraction of the backdrop width.</param>
	/// <param name="restWidth">The sigil's unstruck width, as a fraction of the backdrop width.</param>
	/// <param name="riteHalfWidth">Half the widest a rite can be drawn, as a fraction of the
	/// backdrop width - see <see cref="RiteWidthCap"/>. This is a rite's CENTRE, and a structure
	/// sticks out half its own width past that, so the margin has to hold both. Leaving it out is
	/// what let the deepest rite overlap the sigil on wide windows: the flat margin was 0.03 and
	/// a rite's half-width on such a window is nearer 0.08. Reserving the worst case rather than
	/// the drawn size keeps this non-circular - the drawn size depends on the spacing, which
	/// depends on this.</param>
	public static float RiteRight(float centreX, float restWidth, float riteHalfWidth)
	{
		float clearance = restWidth * 0.5f + SigilMargin + riteHalfWidth;
		return Math.Clamp(centreX - clearance, RiteLeft + 0.05f, 0.55f);
	}

	/// <summary>
	/// Gap between neighbouring rites.
	/// </summary>
	/// <remarks>
	/// The band is shared between the rites STANDING and not between all eight - budgeting for a
	/// parish the keeper will not have for hours left the early ones as specks in slots sized for
	/// someone else.
	/// </remarks>
	public static float RiteSpacing(int standing, float riteRight)
		=> standing > 1 ? (riteRight - RiteLeft) / (standing - 1) : 0.14f;

	/// <summary>Where a rite stands: evenly along the horizon, deepest furthest in. Centred when
	/// there are too few to fill the band.</summary>
	public static float RiteX(int rite, int standing, float riteRight)
	{
		float spacing = RiteSpacing(standing, riteRight);
		float span = (standing - 1) * spacing;
		float left = RiteLeft + (riteRight - RiteLeft - span) * 0.5f;
		return left + rite * spacing;
	}

	/// <summary>
	/// Where the conduits arrive: just inside the sigil's rim, above and a little to the left.
	/// </summary>
	/// <remarks>
	/// Not its centre. A wire that runs to the middle of a disc has to pass over the face of it to
	/// get there, and the face is the one thing on screen the keeper is looking at. Meeting the
	/// rim means the line stops at the edge of the mark and the yield is taken IN, which is also
	/// how it reads: eight conduits terminating on one boundary.
	/// <para>
	/// ABOVE it, because the run comes over the parish and drops straight down into it - the
	/// descent has to be somewhere the structures are not, and there is nothing above the sigil.
	/// This was the lower rim while the route sagged below the horizon, and the approach to it
	/// crossed the row the stoke and ward buttons live on.
	/// </para>
	/// <para>
	/// The offsets are per-axis because the backdrop is not square, so one fraction of the sigil's
	/// resting size would have made a round offset into an oval one.
	/// </para>
	/// </remarks>
	public static Vector2 Intake(Vector2 centre, float restWidth, float restHeight)
		=> new Vector2(centre.X - restWidth * IntakeLeft, centre.Y - restHeight * IntakeRise);

	/// <summary>
	/// The height the conduits arch at.
	/// </summary>
	/// <remarks>
	/// Above everything it has to cross - the tallest crown in the parish, the count above it,
	/// and the point on the sigil the wires are heading for - and never nearer the top edge than
	/// <see cref="Ceiling"/>. One shared height for every conduit rather than one each, because
	/// eight wires at eight different heights reads as a tangle and eight at one reads as a rig.
	/// </remarks>
	/// <param name="highest">The smallest y of anything the arch must clear.</param>
	public static float CanopyY(float highest)
		=> Math.Clamp(highest - Clearance, Ceiling, Horizon - 0.02f);

	/// <summary>How much longer a rounded corner is than the two straight legs it replaces would
	/// be if they met at a point. A quarter-turn quadratic with legs of r is about 1.148r.</summary>
	private const float CornerArc = 1.148f;

	/// <summary>
	/// How much to round the two corners of the run, as a fraction of the backdrop.
	/// </summary>
	/// <remarks>
	/// Bounded by everything it could otherwise eat: half of each vertical leg, a share of the
	/// horizontal one, and - the one that matters - a share of the gap between neighbouring rites,
	/// because the rise happens at a rite's own x and a corner that bulges wider than half the
	/// spacing would lean the wire into the structure next door.
	/// </remarks>
	public static float CornerRadius(Vector2 from, Vector2 to, float canopy, float spacing)
	{
		float run = MathF.Abs(to.X - from.X);
		float rise = from.Y - canopy;
		float fall = to.Y - canopy;
		float limit = MathF.Min(MathF.Min(0.05f, spacing * 0.4f),
			MathF.Min(run * 0.4f, MathF.Min(rise * 0.5f, fall * 0.5f)));
		return MathF.Max(0.0f, limit);
	}

	/// <summary>
	/// A point along the conduit running from a rite's crown to the sigil.
	/// </summary>
	/// <remarks>
	/// <para>
	/// One function, sampled twice: once per frame to lay out the pips that draw the conduit, and
	/// again every frame per bearer to place the bead travelling along it. Sharing the function is
	/// the whole point - a wire drawn from one curve and a bead moved along another drifts apart
	/// the moment either is touched, and a bead visibly off its own wire looks worse than no wire
	/// at all.
	/// </para>
	/// <para>
	/// Up, across, down, with the corners rounded: a cable run rather than a lob. It was a single
	/// cubic with both handles on the canopy first, and that could not be made to work. A cubic's
	/// height profile depends only on where it starts and ends, not on how far it travels - so
	/// every conduit began descending at the same FRACTION of its journey, which for the wire
	/// coming from the far left of the parish meant beginning its descent somewhere over the
	/// middle of it, straight down through the structures in between. Two attempts were made at
	/// capping structure sizes to buy that descent room before it was clear that the descent
	/// simply has to be anchored in POSITION, not in progress. Here it is: the drop happens at the
	/// sigil's x, wherever the wire started.
	/// </para>
	/// <para>
	/// Parameterised by distance along the run rather than by segment, so a bead moves at one
	/// speed the whole way instead of hurrying along the short legs.
	/// </para>
	/// </remarks>
	public static Vector2 Conduit(Vector2 from, Vector2 to, float canopy, float radius, float t)
	{
		float dir = to.X >= from.X ? 1.0f : -1.0f;
		float rise = MathF.Max(0.0f, from.Y - canopy - radius);
		float run = MathF.Max(0.0f, MathF.Abs(to.X - from.X) - radius * 2.0f);
		float fall = MathF.Max(0.0f, to.Y - canopy - radius);
		float corner = radius * CornerArc;
		float total = rise + corner + run + corner + fall;
		if (total <= 0.0f)
		{
			return to;
		}

		float d = Math.Clamp(t, 0.0f, 1.0f) * total;

		// Straight up out of the crown.
		if (d <= rise)
		{
			return new Vector2(from.X, from.Y - d);
		}
		d -= rise;

		// Turning onto the canopy.
		if (d <= corner)
		{
			float k = corner > 0.0f ? d / corner : 1.0f;
			return Bend(new Vector2(from.X, canopy + radius), new Vector2(from.X, canopy),
				new Vector2(from.X + dir * radius, canopy), k);
		}
		d -= corner;

		// Along the canopy.
		if (d <= run)
		{
			return new Vector2(from.X + dir * (radius + d), canopy);
		}
		d -= run;

		// Turning off it, above the sigil.
		if (d <= corner)
		{
			float k = corner > 0.0f ? d / corner : 1.0f;
			return Bend(new Vector2(to.X - dir * radius, canopy), new Vector2(to.X, canopy),
				new Vector2(to.X, canopy + radius), k);
		}
		d -= corner;

		// Straight down into the rim.
		return new Vector2(to.X, canopy + radius + d);
	}

	/// <summary>One rounded corner: a quadratic through the point the two legs would have met at.</summary>
	private static Vector2 Bend(Vector2 a, Vector2 via, Vector2 b, float k)
	{
		float u = 1.0f - k;
		return a * (u * u) + via * (2.0f * u * k) + b * (k * k);
	}

	/// <summary>
	/// How long the conduit is.
	/// </summary>
	/// <remarks>
	/// Summed from the legs rather than by sampling the curve, because the shape is known: two
	/// straights, two corners and a straight. Exact, where sampling a cubic was an estimate.
	/// </remarks>
	public static float ConduitLength(Vector2 from, Vector2 to, float canopy, float radius)
	{
		float rise = MathF.Max(0.0f, from.Y - canopy - radius);
		float run = MathF.Max(0.0f, MathF.Abs(to.X - from.X) - radius * 2.0f);
		float fall = MathF.Max(0.0f, to.Y - canopy - radius);
		return MathF.Max(0.05f, rise + run + fall + radius * CornerArc * 2.0f);
	}
}

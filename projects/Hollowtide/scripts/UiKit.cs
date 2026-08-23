using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A thin layer over <see cref="Ui"/> that makes runtime UI readable: place a box at
/// (x, y, w, h) inside its parent and be done.
/// </summary>
/// <remarks>
/// <para>
/// The engine's rect model is anchors plus offsets plus a pivot - the right model for a
/// layout that has to survive a resize, and the wrong one to retype forty times. Every
/// helper here pins the element to its parent's TOP-LEFT with a top-left pivot, so x and y
/// are plain pixels down and right from the parent's corner, and only the handful of
/// elements that genuinely stretch use <see cref="Stretch"/>.
/// </para>
/// <para>
/// UI space is y-DOWN: (0, 0) is the top-left of the screen, which is also where the mouse
/// reports from, so hit-testing and layout share one coordinate system.
/// </para>
/// </remarks>
public static class UiKit
{
	private static readonly Vector2 kZero = new Vector2(0.0f, 0.0f);

	/// <summary>An image box, positioned from its parent's top-left corner.</summary>
	public static Entity Image(Entity parent, float x, float y, float w, float h, Vector4 colour, float radius = 0.0f)
	{
		Entity e = Ui.CreateImage(parent);
		Reparent(e, parent);
		Ui.SetAnchors(e, kZero, kZero);
		Ui.SetPivot(e, kZero);
		Ui.SetRect(e, x, y, w, h);
		Ui.SetImageColor(e, colour);
		Ui.SetImageCornerRadius(e, radius);
		return e;
	}

	/// <summary>An image pinned to a fraction of its parent, with pixel insets: the shape a
	/// panel that must survive a window resize wants.</summary>
	public static Entity Stretch(Entity parent, Vector2 anchorMin, Vector2 anchorMax,
		Vector2 offsetMin, Vector2 offsetMax, Vector4 colour, float radius = 0.0f)
	{
		Entity e = Ui.CreateImage(parent);
		Reparent(e, parent);
		Ui.SetAnchors(e, anchorMin, anchorMax);
		Ui.SetPivot(e, kZero);
		Ui.SetOffsets(e, offsetMin, offsetMax);
		Ui.SetImageColor(e, colour);
		Ui.SetImageCornerRadius(e, radius);
		return e;
	}

	/// <summary>A single line of text. Wrap is off by default: these are laid out by hand, and
	/// a wrapped line silently changes the height every row after it was placed against.</summary>
	public static Entity Text(Entity parent, string text, float x, float y, float w, float h,
		float size, Vector4 colour, UiHAlign hAlign = UiHAlign.Left, string font = Palette.Body)
	{
		Entity e = Ui.CreateText(parent, text);
		Reparent(e, parent);
		Ui.SetAnchors(e, kZero, kZero);
		Ui.SetPivot(e, kZero);
		Ui.SetRect(e, x, y, w, h);
		Ui.SetFont(e, font);
		Ui.SetFontSize(e, size);
		Ui.SetTextColor(e, colour);
		Ui.SetTextAlign(e, hAlign, UiVAlign.Middle);
		Ui.SetTextWrap(e, false);
		Ui.SetText(e, text);
		return e;
	}

	/// <summary>Text that wraps inside its box - blurbs and the offline report, where the
	/// height is the thing that gives and the layout below it can take it.</summary>
	public static Entity Paragraph(Entity parent, string text, float x, float y, float w, float h,
		float size, Vector4 colour, string font = Palette.Body)
	{
		Entity e = Text(parent, text, x, y, w, h, size, colour, UiHAlign.Left, font);
		Ui.SetTextWrap(e, true);
		Ui.SetTextAlign(e, UiHAlign.Left, UiVAlign.Top);
		return e;
	}

	/// <summary>
	/// A clickable box with a label. Selectable, so it joins the engine's navigation and
	/// answers a d-pad and Enter as well as the mouse - there is no per-screen highlight
	/// index anywhere in this game.
	/// </summary>
	public static Button MakeButton(Entity parent, string label, float x, float y, float w, float h,
		float size = 18.0f, string font = Palette.Body)
	{
		Entity root = Image(parent, x, y, w, h, Palette.Row, 4.0f);
		Ui.SetSelectable(root);
		Entity text = Text(root, label, 0.0f, 0.0f, w, h, size, Palette.TextBright, UiHAlign.Center, font);
		// The label stretches with the box so a button that is later resized keeps its text
		// centred, rather than centred on the width it happened to be created at.
		Ui.SetAnchors(text, kZero, new Vector2(1.0f, 1.0f));
		Ui.SetOffsets(text, kZero, kZero);
		return new Button(root, text);
	}

	/// <summary>Ui.CreateImage/CreateText parent to a CANVAS. Anything nested deeper is
	/// re-parented here, in one place, so a helper cannot forget to.</summary>
	private static void Reparent(Entity child, Entity parent)
	{
		if (parent.IsValid)
		{
			child.SetParent(parent);
		}
	}
}

/// <summary>
/// A button, in either of the two shapes this game uses.
/// </summary>
/// <remarks>
/// <para>
/// Chrome authored in the scene is a real engine <c>UI Button</c>: it owns its own label and
/// its own focus colours, so the engine styles the highlight and nothing here has to. Those
/// are bound with <see cref="Find"/> and leave <see cref="Label"/> invalid.
/// </para>
/// <para>
/// A ledger row cannot be one, because a row is four columns of text and a UI Button has one
/// label. Those stay an image with a selectable and separate text children, built by
/// <see cref="UiKit.MakeButton"/>. Both answer the same calls; the only difference is where
/// the label lives, which is what the <see cref="Label"/> check below is deciding.
/// </para>
/// </remarks>
public readonly struct Button
{
	public readonly Entity Root;
	/// <summary>The separate label entity of a composed button, or invalid for an authored
	/// UI Button, which carries its label in the component.</summary>
	public readonly Entity Label;

	public Button(Entity root, Entity label)
	{
		Root = root;
		Label = label;
	}

	/// <summary>Bind an authored UI Button from the scene by name.</summary>
	public static Button Find(string name) => new Button(Scene.Find(name), default);

	public bool IsValid => Root.IsValid;

	/// <summary>True on the frame it was clicked, pressed with Enter/Space while focused, or
	/// activated with a pad. One call covers all three because the engine's navigation pass
	/// resolves them into one answer - and consumes the key, so the game underneath does not
	/// also see it.</summary>
	public bool Activated => Root.IsValid && Ui.WasActivated(Root);

	public bool Hovered => Root.IsValid && Ui.IsHovered(Root);
	public bool Focused => Root.IsValid && Ui.IsFocused(Root);

	public void SetLabel(string text)
	{
		if (Label.IsValid)
		{
			Ui.SetText(Label, text);
		}
		else if (Root.IsValid)
		{
			Ui.SetButtonLabel(Root, text);
		}
	}

	public void SetLabelColour(Vector4 colour)
	{
		if (Label.IsValid)
		{
			Ui.SetTextColor(Label, colour);
		}
		else if (Root.IsValid)
		{
			// Only the resting colour. The engine picks text_color_focused itself while the
			// button holds focus, so writing that here would fight the highlight.
			Root.Component("UI Button").SetVector4("text_color", colour);
		}
	}

	public void SetColour(Vector4 colour)
	{
		if (!Root.IsValid)
		{
			return;
		}
		if (Label.IsValid)
		{
			Ui.SetImageColor(Root, colour);
		}
		else
		{
			Root.Component("UI Button").SetVector4("bg_color", colour);
		}
	}

	/// <summary>Dim and un-navigate a button that cannot be used, so a locked row is skipped
	/// by the d-pad instead of being focusable and inert.</summary>
	public void SetEnabled(bool enabled)
	{
		if (Root.IsValid)
		{
			Ui.SetInteractable(Root, enabled);
		}
	}

	public void SetActive(bool active)
	{
		if (Root.IsValid)
		{
			Root.SetActive(active);
		}
	}

	/// <summary>Per-frame styling for whether the button can be used at all. An authored button
	/// already answers focus on its own, so this only ever expresses AFFORDABILITY - the lit
	/// state is the engine's business, and a composed row button gets it applied here.</summary>
	public void Style(bool affordable, Vector4 hot, Vector4 cold, Vector4 dead)
	{
		if (!Root.IsValid)
		{
			return;
		}
		bool lit = affordable && Label.IsValid && (Hovered || Focused);
		SetColour(!affordable ? dead : lit ? hot : cold);
		SetLabelColour(affordable ? Palette.TextBright : Palette.TextFaint);
	}
}

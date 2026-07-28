using System.Numerics;
using AetherCore;
using Xunit;

namespace AetherCore.Tests;

/// <summary>
/// Where a world-tracking label puts itself, and when it takes itself off the screen.
/// </summary>
public sealed class WorldLabelTests : SdkTestBase
{
    // The sentinel Camera.WorldToScreen returns for a point behind the camera. Repeated
    // here on purpose: the label's cull is a comparison against this exact value, and a
    // test that read the constant from the code under test would not notice it drifting.
    private static readonly Vector2 BehindCamera = new(-1.0f, -1.0f);

    [Fact]
    public void AnOutlinedLabelCreatesFourBackingCopiesUnderOneText()
    {
        WorldLabel label = new(width: 100.0f, height: 20.0f);

        Assert.Equal(5, Engine.UiElements.Count);
        // The real text is created LAST so it draws over its backing copies.
        Assert.Equal(Engine.UiElements[4], label.Element);
    }

    [Fact]
    public void OutlineZeroCreatesNoExtraElements()
    {
        WorldLabel label = new(outlineWidth: 0.0f);

        Assert.Single(Engine.UiElements);
        Assert.Equal(Engine.UiElements[0], label.Element);
    }

    [Fact]
    public void EveryElementIsAnchoredTopLeftWithACentrePivot()
    {
        // WorldToScreen hands back top-left-origin pixels, and a centre-anchored element
        // would read them as offsets from the middle of the screen.
        WorldLabel label = new(width: 100.0f, height: 20.0f);

        foreach (Entity element in Engine.UiElements)
        {
            UiCall anchors = Assert.Single(Engine.CallsOn(element, "SetAnchors"));
            Assert.Equal(new Vector4(0.0f, 0.0f, 0.0f, 0.0f), new Vector4(anchors.A, anchors.B, anchors.C, anchors.D));

            UiCall pivot = Assert.Single(Engine.CallsOn(element, "SetPivot"));
            Assert.Equal(0.5f, pivot.A);
            Assert.Equal(0.5f, pivot.B);
        }

        Assert.NotEqual(default, label.Element);
    }

    [Fact]
    public void TrackPutsTheTextAtTheProjectedPoint()
    {
        Engine.Project = _ => new Vector2(640.0f, 360.0f);
        WorldLabel label = new(width: 100.0f, height: 20.0f, outlineWidth: 0.0f);

        label.Track(new Vector3(3.0f, 4.0f, 0.0f), "Alice");

        UiCall rect = Engine.LastCall(label.Element, "SetRect");
        Assert.Equal(640.0f, rect.A);
        Assert.Equal(360.0f, rect.B);
        Assert.Equal(100.0f, rect.C);
        Assert.Equal(20.0f, rect.D);
    }

    [Fact]
    public void TrackWritesTheTextOnEveryCopy()
    {
        Engine.Project = _ => new Vector2(10.0f, 20.0f);
        WorldLabel label = new();

        label.Track(Vector3.Zero, "Alice");

        foreach (Entity element in Engine.UiElements)
        {
            UiCall text = Engine.LastCall(element, "SetText");
            Assert.Equal("Alice", text.Text);
        }
    }

    [Fact]
    public void TrackOffsetsTheBackingCopiesDiagonallyByTheOutlineWidth()
    {
        Engine.Project = _ => new Vector2(200.0f, 100.0f);
        WorldLabel label = new(width: 100.0f, height: 20.0f, outlineWidth: 2.0f);

        label.Track(Vector3.Zero, "Alice");

        Vector2[] expected =
        [
            new(198.0f, 98.0f),
            new(202.0f, 98.0f),
            new(198.0f, 102.0f),
            new(202.0f, 102.0f),
        ];
        for (int i = 0; i < expected.Length; i++)
        {
            UiCall rect = Engine.LastCall(Engine.UiElements[i], "SetRect");
            Assert.Equal(expected[i], new Vector2(rect.A, rect.B));
        }

        // ...and the real text sits exactly on the projected point, not offset with them.
        UiCall textRect = Engine.LastCall(label.Element, "SetRect");
        Assert.Equal(new Vector2(200.0f, 100.0f), new Vector2(textRect.A, textRect.B));
    }

    [Fact]
    public void TrackShowsTheLabelWhenThePointIsInFrontOfTheCamera()
    {
        Engine.Project = _ => new Vector2(200.0f, 100.0f);
        WorldLabel label = new();

        label.Track(Vector3.Zero, "Alice");

        foreach (Entity element in Engine.UiElements)
        {
            Assert.True(Engine.ActiveState[element.Id]);
        }
    }

    [Fact]
    public void TrackHidesEveryElementWhenThePointIsBehindTheCamera()
    {
        Engine.Project = _ => BehindCamera;
        WorldLabel label = new();

        label.Track(Vector3.Zero, "Alice");

        foreach (Entity element in Engine.UiElements)
        {
            Assert.False(Engine.ActiveState[element.Id]);
        }

        Assert.NotEqual(default, label.Element);
    }

    [Fact]
    public void TrackDoesNotMoveAHiddenLabelToTheSentinelCorner()
    {
        // The whole point of the cull: (-1, -1) is not a position, and writing it would
        // pin the label to the top-left corner of the screen instead of hiding it.
        Engine.Project = _ => BehindCamera;
        WorldLabel label = new();
        int rectCallsFromConstruction = Engine.UiCalls.FindAll(c => c.Op == "SetRect").Count;

        label.Track(Vector3.Zero, "Alice");

        Assert.Equal(rectCallsFromConstruction, Engine.UiCalls.FindAll(c => c.Op == "SetRect").Count);
    }

    [Fact]
    public void ALabelComesBackWhenThePointReturnsInFrontOfTheCamera()
    {
        WorldLabel label = new(outlineWidth: 0.0f);

        Engine.Project = _ => BehindCamera;
        label.Track(Vector3.Zero, "Alice");
        Assert.False(Engine.ActiveState[label.Element.Id]);

        Engine.Project = _ => new Vector2(50.0f, 60.0f);
        label.Track(Vector3.Zero, "Alice");

        Assert.True(Engine.ActiveState[label.Element.Id]);
        UiCall rect = Engine.LastCall(label.Element, "SetRect");
        Assert.Equal(new Vector2(50.0f, 60.0f), new Vector2(rect.A, rect.B));
    }

    [Fact]
    public void SetFontAndSetFontSizeReachTheOutlineToo()
    {
        // A backing copy laid out differently from the string it sits behind reads as a
        // blur, so these two must never be applied to the text alone.
        WorldLabel label = new();

        label.SetFont("IBMPlexMono-Italic");
        label.SetFontSize(18.0f);

        foreach (Entity element in Engine.UiElements)
        {
            Assert.Equal("IBMPlexMono-Italic", Assert.Single(Engine.CallsOn(element, "SetFont")).Text);
            Assert.Equal(18.0f, Assert.Single(Engine.CallsOn(element, "SetFontSize")).A);
        }

        Assert.NotEqual(default, label.Element);
    }

    [Fact]
    public void SetColorLeavesTheOutlineAlone()
    {
        WorldLabel label = new();
        int outlineColorCalls = Engine.UiCalls.FindAll(c => c.Op == "SetTextColor").Count;

        label.SetColor(new Vector4(1.0f, 0.0f, 0.0f, 1.0f));

        Assert.Equal(outlineColorCalls + 1, Engine.UiCalls.FindAll(c => c.Op == "SetTextColor").Count);
        UiCall applied = Engine.LastCall(label.Element, "SetTextColor");
        Assert.Equal(new Vector4(1.0f, 0.0f, 0.0f, 1.0f), new Vector4(applied.A, applied.B, applied.C, applied.D));
    }

    [Fact]
    public void DestroyTakesEveryElementWithIt()
    {
        WorldLabel label = new();

        label.Destroy();

        Assert.Equal(5, Engine.Destroyed.Count);
        Assert.Equal(default, label.Element);
    }

    [Fact]
    public void DestroyIsSafeTwice()
    {
        WorldLabel label = new();

        label.Destroy();
        label.Destroy();

        Assert.Equal(5, Engine.Destroyed.Count);
    }

    [Fact]
    public void TrackAfterDestroyDoesNothing()
    {
        WorldLabel label = new();
        label.Destroy();
        int callsBefore = Engine.UiCalls.Count;

        label.Track(Vector3.Zero, "Alice");

        Assert.Equal(callsBefore, Engine.UiCalls.Count);
    }
}

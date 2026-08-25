using AetherCore;

namespace AetherGame;

/// <summary>
/// The three presentation settings, wherever they are shown.
/// </summary>
/// <remarks>
/// Bound by name from a prefix and driven by one Read, because there are two settings pages -
/// the threshold's and the vigil's - and they are the same settings. Written twice, the second
/// copy is where a setting gets added to one page and not the other, or saved on one page and
/// not the other. This is the one that had already started: type size existed only on a screen
/// with no parish text on it, which is the text the complaint was about.
/// </remarks>
public sealed class VigilSettings
{
	private Entity _whisperToggle;
	private Entity _shakeSlider;
	private Entity _shakeLabel;
	private Entity _typeSlider;
	private Entity _typeLabel;

	/// <summary>Find the controls under a screen's prefix and seed them from the save.</summary>
	public void Bind(string prefix)
	{
		_whisperToggle = Scene.Find(prefix + "WhisperToggle");
		_shakeSlider = Scene.Find(prefix + "ShakeSlider");
		_shakeLabel = Scene.Find(prefix + "ShakeLabel");
		_typeSlider = Scene.Find(prefix + "TypeSlider");
		_typeLabel = Scene.Find(prefix + "TypeLabel");

		Ui.SetToggle(_whisperToggle, SaveSystem.ShowWhispers);
		Ui.SetSliderValue(_shakeSlider, SaveSystem.DreadShake);
		Ui.SetSliderValue(_typeSlider, SaveSystem.TypeScale);
	}

	/// <summary>
	/// Take whatever changed, and say what everything is.
	/// </summary>
	/// <remarks>
	/// Settings are persisted the moment they change rather than on the way out, so a player who
	/// alt-F4s off this screen keeps what they just set.
	/// </remarks>
	public void Read()
	{
		if (Ui.WasChanged(_whisperToggle))
		{
			SaveSystem.ShowWhispers = Ui.GetToggle(_whisperToggle);
			SaveSystem.Save();
		}
		if (Ui.WasChanged(_shakeSlider))
		{
			SaveSystem.DreadShake = Ui.GetSliderValue(_shakeSlider);
			SaveSystem.Save();
		}
		if (Ui.WasChanged(_typeSlider))
		{
			SaveSystem.TypeScale = Ui.GetSliderValue(_typeSlider);
			// Applied on the spot rather than on the way out. Type size is the one setting whose
			// effect IS the thing being looked at while it is chosen - a slider that only takes
			// effect on the next launch cannot be judged at all.
			TypeScale.Apply(SaveSystem.TypeScale);
			SaveSystem.Save();
		}
		Ui.SetText(_shakeLabel, "UNSTEADINESS " + Numbers.Percent(SaveSystem.DreadShake / 1.5f));
		Ui.SetText(_typeLabel, "TYPE SIZE " + Numbers.Percent(SaveSystem.TypeScale / Typography.Authored));
	}
}

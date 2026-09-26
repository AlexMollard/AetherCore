using System;
using System.Diagnostics;
using System.Numerics;
using AetherCore;

/// Pre-rendered FMV playback (FMV/*.PSS, engine/movies.md): tw-extract --movies writes each movie as
/// movies/<NAME>/f00001.jpg... (512x288, 25 fps) plus movies/<NAME>.wav (the English PCM track).
/// The game freezes (Time.Pause) under a full-screen black canvas; the frame image swaps on a
/// Stopwatch clock started together with the audio, so video tracks wall time (= the audio clock)
/// and a late frame is skipped, never delayed. Hold Triangle / Cross or Enter / Space 0.5 s to skip.
public sealed class TwinsanityMovie
{
	private const float Fps = 25.0f;
	private const float SkipHold = 0.5f;
	private const string Dir = "project://assets/movies/";

	private Entity _canvas, _image, _bar;
	private readonly TwinsanitySkipPrompt _prompt = new();
	private string _name = "";
	private int _frames, _shown, _missed;
	private double _worstGap, _lastShow;
	private readonly Stopwatch _clock = new();
	private float _held, _lastUnscaled = -1.0f;
	private Action? _done;

	public bool Playing { get; private set; }

	/// <summary>Start <paramref name="name"/> (e.g. "H01_A"); <paramref name="done"/> runs when it ends or is skipped.
	/// Returns false (and runs nothing) when the movie was not extracted.</summary>
	public bool Play(string name, Action? done = null)
	{
		_frames = Assets.List($"{Dir}{name}/*.jpg").Length;
		if (_frames == 0)
		{
			Log.Warn($"[Movie] {name} not extracted - run tw-extract --movies {name}");
			return false;
		}
		if (!_canvas.IsValid)
		{
			_canvas = Ui.CreateCanvas();
			_canvas.MarkTransient();
			Entity black = Ui.CreateImage(_canvas);
			Ui.SetAnchors(black, Vector2.Zero, Vector2.One);
			Ui.SetOffsets(black, Vector2.Zero, Vector2.Zero);
			Ui.SetImageColor(black, new Vector4(0.0f, 0.0f, 0.0f, 1.0f));
			// ponytail: stretched to the window (16:9 frames; exact on 16:9 windows). A pillarbox needs a
			// screen-size query the script API does not expose.
			_image = Ui.CreateImage(_canvas);
			Ui.SetAnchors(_image, Vector2.Zero, Vector2.One);
			Ui.SetOffsets(_image, Vector2.Zero, Vector2.Zero);
			Ui.SetImageColor(_image, Vector4.One);
			_bar = Ui.CreateImage(_canvas); // layout box for the skip prompt: the bottom 15%, as a letterbox bar
			Ui.SetAnchors(_bar, new Vector2(0.0f, 0.85f), Vector2.One);
			Ui.SetOffsets(_bar, Vector2.Zero, Vector2.Zero);
			Ui.SetImageColor(_bar, Vector4.Zero);
		}
		_name = name;
		_done = done;
		_shown = _missed = 0;
		_worstGap = 0.0;
		_held = 0.0f;
		_lastUnscaled = -1.0f;
		_canvas.SetActive(true);
		Playing = true;
		Time.Pause();
		Audio.PlayMusic($"{Dir}{name}.wav", 0.0f, false, 1.0f);
		_clock.Restart();
		_lastShow = 0.0;
		Show(1);
		Log.Info($"[Movie] {name}: {_frames} frames ({_frames / Fps:F2} s)");
		return true;
	}

	public void Update()
	{
		if (!Playing)
		{
			return;
		}
		float now = Time.UnscaledTime;
		float dt = _lastUnscaled < 0.0f ? 0.0f : now - _lastUnscaled;
		_lastUnscaled = now;
		bool held = Gamepad.IsDown(GamepadButton.Y) || Gamepad.IsDown(GamepadButton.A) || Input.IsKeyDown(Key.Enter) || Input.IsKeyDown(Key.Space);
		_held = held ? _held + dt : 0.0f;
		_prompt.Update(_canvas, _bar, true);
		double t = _clock.Elapsed.TotalSeconds;
		int frame = (int)(t * Fps) + 1;
		if (_held >= SkipHold || frame > _frames)
		{
			Finish(_held >= SkipHold);
			return;
		}
		if (frame != _shown)
		{
			_missed += frame - _shown - 1;
			Show(frame);
		}
	}

	private void Show(int frame)
	{
		double t = _clock.Elapsed.TotalSeconds;
		if (_shown > 0)
		{
			_worstGap = Math.Max(_worstGap, t - _lastShow);
		}
		_lastShow = t;
		_shown = frame;
		// The previous frame's texture drops its last registry ref here (the image holds one ref).
		Ui.SetImageTexture(_image, $"{Dir}{_name}/f{frame:D5}.jpg");
	}

	private void Finish(bool skipped)
	{
		double t = _clock.Elapsed.TotalSeconds;
		// A/V drift: the video clock vs the audio's (both started on the same frame; audio plays at wall rate).
		double drift = _shown / Fps - t;
		Log.Info($"[Movie] {_name} {(skipped ? "skipped" : "ended")} at {t:F3} s: {_shown}/{_frames} frames, {_missed} missed, worst frame gap {_worstGap * 1000.0:F1} ms, video-audio drift {drift * 1000.0:F1} ms");
		Playing = false;
		Audio.StopMusic(0.0f);
		Ui.SetImageTexture(_image, string.Empty);
		_prompt.Update(_canvas, _bar, false);
		_canvas.SetActive(false);
		Time.Resume();
		Action? done = _done;
		_done = null;
		done?.Invoke();
	}
}

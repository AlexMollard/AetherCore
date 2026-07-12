using System.Collections;

namespace AetherCore;

/// <summary>Yield this from a coroutine to pause for a number of seconds.</summary>
public sealed class WaitForSeconds
{
    public readonly float Seconds;
    public WaitForSeconds(float seconds) => Seconds = seconds;
}

/// <summary>Yield this from a coroutine to pause for a number of frames.</summary>
public sealed class WaitForFrames
{
    public readonly int Frames;
    public WaitForFrames(int frames) => Frames = frames;
}

/// <summary>
/// A running coroutine started with <see cref="EntityScript.StartCoroutine"/>.
/// Advances one step per frame, honouring <see cref="WaitForSeconds"/> /
/// <see cref="WaitForFrames"/>; <c>yield return null</c> resumes next frame.
/// </summary>
public sealed class Coroutine
{
    private readonly IEnumerator _routine;
    private bool _stopped;
    private float _waitSeconds;
    private int _waitFrames;

    internal Coroutine(IEnumerator routine) => _routine = routine;

    /// <summary>True once the coroutine has finished or been stopped.</summary>
    public bool IsDone { get; private set; }

    /// <summary>Stop the coroutine; it will not advance again.</summary>
    public void Stop() => _stopped = true;

    // Returns false when the coroutine is finished/stopped and should be dropped.
    internal bool Tick(float deltaTime)
    {
        if (_stopped)
        {
            IsDone = true;
            return false;
        }
        if (_waitSeconds > 0.0f)
        {
            _waitSeconds -= deltaTime;
            if (_waitSeconds > 0.0f)
            {
                return true;
            }
            _waitSeconds = 0.0f;
        }
        if (_waitFrames > 0)
        {
            _waitFrames--;
            return true;
        }

        bool moved;
        try
        {
            moved = _routine.MoveNext();
        }
        catch (System.Exception ex)
        {
            Log.Error($"Coroutine faulted: {ex.Message}");
            IsDone = true;
            return false;
        }
        if (!moved)
        {
            IsDone = true;
            return false;
        }

        switch (_routine.Current)
        {
            case WaitForSeconds w:
                _waitSeconds = w.Seconds;
                break;
            case WaitForFrames f:
                _waitFrames = f.Frames;
                break;
            default:
                // null or any unrecognised yield resumes on the next frame.
                break;
        }
        return true;
    }
}

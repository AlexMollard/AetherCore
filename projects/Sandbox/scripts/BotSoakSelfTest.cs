using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A discrimination check for <see cref="BotSoakMonitor"/>, not part of the shipped
/// bot fleet: proves the anomaly detector can actually FAIL, not just always pass -
/// the same reasoning a security test suite applies with a known-bad input. Never
/// referenced by any scene's authored [[entities.scripts]]; attach it live to any
/// entity via the control API (<c>scene.add_script {"id": &lt;any entity&gt;, "type":
/// "BotSoakSelfTest"}</c>) while BotTest.scene.toml is playing.
///
/// RUN THIS WITH THE BOT FLEET'S BotBrain SCRIPTS REMOVED (scene.remove_script on
/// each Bot), not just alongside them. A live bot will actively "fix" three of the
/// four provocations below within about a second - grabbing a nudged crate and
/// overwriting its velocity with its own hold spring, or the crate simply bouncing
/// off a wall and settling - long before the cause was ever confirmed as the
/// detector, not the provocation, surviving. Reload the scene (which discards this
/// script instance along with every provocation) before starting a real timed run.
///
/// SUSTAINED, NOT ONE-SHOT: a single Physics.SetLinearVelocity is not enough - a
/// wall collision or a bot's own drive spring can resolve it away within one
/// physics substep, well before BotSoakMonitor's once-a-second check ever samples
/// it. This re-applies every provocation every frame for <see cref="SustainSeconds"/>,
/// so at least one check tick is guaranteed to observe the broken state.
///
/// Exercises four of BotSoakMonitor's six anomaly kinds (StuckLoop and
/// EntityCountGrowth are behavioural over time, not a one-shot state corruption,
/// and are not cheap to provoke this way):
///  - Prop Cube 1: velocity forced to a large but finite value -> InsaneSpeed.
///  - Prop Cube 2: velocity forced to NaN -> NonFiniteVelocity, then (once that NaN
///    integrates into position) NonFinitePosition too - both kinds from one target.
///  - Prop Cube 3: velocity forced hard downward, fast enough that this scene's
///    non-CCD ground collider misses the discrete overlap check for at least one
///    substep -> tunnels through -> FellThroughFloor.
///  - Prop Sphere 1: velocity forced hard sideways, fast enough to tunnel clean
///    through a wall in one substep the same way -> OutOfBounds.
/// </summary>
public sealed class BotSoakSelfTest : EntityScript
{
    public float DelaySeconds = 1.0f;
    public float SustainSeconds = 4.0f;

    private float _timer;
    private bool _announced;

    public override void OnUpdate(float deltaTime)
    {
        _timer += deltaTime;
        if (_timer < DelaySeconds || _timer > DelaySeconds + SustainSeconds)
        {
            return;
        }

        if (!_announced)
        {
            _announced = true;
            Log.Info("[Sandbox] BotSoakSelfTest: provocations started (sustained "
                    + $"{SustainSeconds:0}s) - expect four SOAK_ANOMALY lines, then "
                    + "reload the scene before any real timed run.");
        }

        Provoke("Prop Cube 1", new Vector3(500.0f, 0.0f, 0.0f));
        Provoke("Prop Cube 2", new Vector3(float.NaN, 0.0f, 0.0f));
        Provoke("Prop Cube 3", new Vector3(0.0f, -300.0f, 0.0f));
        Provoke("Prop Sphere 1", new Vector3(300.0f, 0.0f, 0.0f));
    }

    private static void Provoke(string targetName, Vector3 velocity)
    {
        Entity target = Scene.Find(targetName);
        if (target.IsValid)
        {
            Physics.SetLinearVelocity(target, velocity);
        }
    }
}

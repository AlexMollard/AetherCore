using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Crash, moving the way he does in the original PAL game.
///
/// The numbers are not tuned by feel. They are the game's own per-character tuning, Crash's
/// instance floats (DefaultEnums.CharacterInstanceFloats, handed over by TwinsanityLevel from the
/// level manifest), and the rules that apply them were measured frame by frame in the original
/// game on the CrashModded PCSX2 rig (logs/tw_measure.py). What the rig showed:
///   - On the ground, velocity snaps to the stick: full speed on the first frame, and an instant
///     turn or reversal. Letting go slows him at 50 m/s^2. Below half deflection nothing happens;
///     below full deflection (a diagonal included) he walks at WalkSpeed, otherwise he runs.
///   - A jump launches at JumpHeight and always reaches the same apex: there is no variable jump
///     height. Rising gravity is JumpArcUnk18, falling gravity AirGravity.
///   - In the air, horizontal velocity chases stick * JumpAirSpeed at 50 m/s^2.
///   - A second jump launches at DoubleJumpHeight, rising under DoubleJumpUnk22.
///   - The slide runs at SlideSpeed for 0.24 s, stops over 0.1 s, then leaves him crouched for 0.3 s.
///     A slide jump launches at SlideJumpUnk25 under SlideJumpUnk26 gravity, keeping SlideJumpUnk24
///     of horizontal speed.
///   - The body slam hangs for BodyslamHangTime, drifting up after 0.16 s, then drops under
///     BodyslamGravityForce.
/// The logic steps at the game's 50 Hz, so frame-counted behaviour lands on the same frames.
///
/// Animations are the game's own, extracted per animation slot ("aNNN"). The slot each move plays
/// comes from Crash's behaviour scripts (act_CRASH.states.json).
///
/// The model is a separate root entity, not a child: the Character Controller writes this entity's
/// transform on its own schedule, so a child would be moved twice.
/// </summary>
public enum DeathKind { Generic, Drown, Explode, Fall }

public sealed class CrashPlayer : EntityScript
{
	public string ModelPath = "project://assets/models/objects/act_CRASH/act_CRASH_0.gltf";
	public float ModelYawOffset = 180.0f;
	public float ModelTurnRate = 1440.0f;

	public float CameraDistance = 7.5f;
	public float CameraPitch = 20.0f;
	public float CameraTargetHeight = 1.4f;
	public float MouseSensitivity = 0.15f;
	public float StickTurnSpeed = 160.0f;

	// CharacterInstanceFloats defaults: Crash's values from the beach instance, used until Configure.
	private float _airGravity = 50.0f;
	private float _walkSpeed = 2.5f;
	private float _runSpeed = 9.0f;
	private float _spinLength = 0.4f;
	private float _spinDelay = 0.15f;
	private float _jumpAirSpeed = 8.0f;
	private float _jumpHeight = 13.0f;
	private float _jumpRiseGravity = 37.556f;
	private float _edgeSpeed = 8.0f;
	private float _doubleJumpHeight = 16.0f;
	private float _doubleJumpGravity = 64.0f;
	private float _slideJumpSpeed = 11.0f;
	private float _slideJumpHeight = 5.0f;
	private float _slideJumpGravity = 10.0f;
	private float _slamHang = 0.4f;
	private float _slamGravity = 400.0f;
	private float _crawlSpeed = 1.75f;
	private float _slideSpeed = 18.0f;

	// Measured, not in the float table.
	private const float Tick = 0.02f;
	private const float StickDeadZone = 0.5f;
	private const float StickRun = 0.97f;
	private const float BrakeRate = 50.0f;
	private const float AirAccel = 50.0f;
	private const float SlideDelay = 0.04f;
	private const float SlideHold = 0.24f;
	private const float SlideStop = 0.1f;
	private const float SlideCrouch = 0.3f;
	private const float SlideJumpFallAt = -0.4f;
	private const float SlamStill = 0.16f;
	private const float SlamRise = 5.0f;
	private const float SlamDropStart = 2.0f;
	private const float SlamLandLock = 0.3f;
	private const float IdleFidgetAfter = 10.0f;
	private const float HeightGain = 30.0f;
	private const int GroundGraceTicks = 2;
	// On the ground the script owns velocity outright (SetVelocity), which skips the engine's own
	// press into the floor; with a vertical speed of exactly 0 a centimetre of float over a sand
	// crest never closed, the grace ran out and the next press was spent as a double jump. Press
	// along the ground normal instead: the contact solve removes all of it, so there is no creep.
	private const float GroundPress = 2.0f;

	private enum State { Ground, Crouch, Slide, Air, SlamHang, SlamDrop, SlamLand }
	private enum Arc { Fall, Jump, DoubleJump, SlideJump, Bounce }

	public bool IsSpinning => _spinTime > 0.0f;
	public bool IsSlamming => _state is State.SlamDrop or State.SlamLand;
	public bool IsSliding => _state == State.Slide;
	public Vector3 Velocity => new(_horizontal.X, _vy, _horizontal.Z);
	public bool IsGrounded => _state is State.Ground or State.Crouch or State.Slide or State.SlamLand;
	public float Facing => _facing;

	private Entity _model;
	private Entity _camera;
	private float _facing;
	private float _modelYaw;
	private float _cameraYaw;
	private bool _control = true;

	private State _state = State.Ground;
	private Arc _arc;
	private Vector3 _horizontal;
	private Vector3 _moveDir = new(0.0f, 0.0f, -1.0f);
	private float _vy;
	// The height the game's own 50 Hz integration would have him at (see TrackHeight).
	private float _airY;
	private float _stateTime;
	private float _spinTime;
	private float _spinCooldown;
	private bool _doubleJumped;
	private float _accumulator;
	private bool _jumpLatch;
	private bool _spinLatch;
	private bool _crouchLatch;
	private bool _spaceWasDown;
	private bool _spinWasDown;
	private bool _crouchWasDown;
	// Ground-loss grace: CharacterController.IsGrounded flickers false for a tick on bumps and
	// slopes (60 Hz physics thread vs our 50 Hz tick), which used to eat jump presses. The
	// original lets Crash jump for a few frames after leaving the ground (the edge jump, whose
	// horizontal speed is _edgeSpeed), so a 2-tick grace is faithful and fixes the flicker.
	private int _groundGrace;
	// Ticks a spent-double-jump press stays live for the landing (see State.Air).
	private int _jumpBuffer;
	private float _idleTime;
	private float _oneShotLeft;
	private string _landClip = "";
	private bool _dead;
	private (string Clip, float Time)[] _deathSeq = Array.Empty<(string, float)>();
	private int _deathIndex;
	private float _deathTimer;
	private float _hurtLeft;
	/// <summary>The original's IsPushingObject: set each frame by TwinsanityMechanics while Crash
	/// walks into a pushable, so walk/run play the push clips.</summary>
	public bool Pushing;
	private string _spinBase = "a008";

	private readonly Dictionary<string, int> _clips = new();
	private readonly System.Random _random = new();
	private string _clip = "";

	/// <summary>Crash's CharacterInstanceFloats from the level's spawn instance.</summary>
	public void Configure(float[] f)
	{
		float At(int i, float fallback) => i < f.Length ? f[i] : fallback;
		_airGravity = At(1, _airGravity);
		_walkSpeed = At(6, _walkSpeed);
		_runSpeed = At(7, _runSpeed);
		_spinLength = At(10, _spinLength);
		_spinDelay = At(11, _spinDelay);
		_jumpAirSpeed = At(15, _jumpAirSpeed);
		_jumpHeight = At(16, _jumpHeight);
		_jumpRiseGravity = At(17, _jumpRiseGravity);
		_edgeSpeed = At(19, _edgeSpeed);
		_doubleJumpHeight = At(20, _doubleJumpHeight);
		_doubleJumpGravity = At(21, _doubleJumpGravity);
		_slideJumpSpeed = At(23, _slideJumpSpeed);
		_slideJumpHeight = At(24, _slideJumpHeight);
		_slideJumpGravity = At(25, _slideJumpGravity);
		_slamHang = At(31, _slamHang);
		_slamGravity = At(33, _slamGravity);
		_crawlSpeed = At(40, _crawlSpeed);
		_slideSpeed = At(44, _slideSpeed);
	}

	public override void OnAttach()
	{
		_model = World.Create();
		_model.Name = "Crash Model";
		_model.AddTransform();
		_model.Position = Self.Position;
		_model.LoadModel(ModelPath);

		_facing = Self.EulerDegrees.Y;
		_modelYaw = _facing;
		_cameraYaw = _facing;
		_camera = Camera.CreateOrbit(Self.Position + new Vector3(0.0f, 3.0f, 6.0f), Self.Position, 60.0f);
		_camera.Name = "Crash Camera";
		Camera.SetMain(_camera);
		PlaceCamera();
		PlaceModel(0.0f);
	}

	public override void OnDetach()
	{
		Input.CursorLockRequested = false;
	}

	public override void OnUpdate(float deltaTime)
	{
		Input.CursorLockRequested = true;

		if (_dead)
		{
			StepDeath(deltaTime);
			return;
		}
		if (_hurtLeft > 0.0f)
		{
			_hurtLeft -= deltaTime;
		}
		Look(deltaTime);

		// Edges are derived here from the down states rather than taken from IsKeyPressed: the
		// engine's pressed edge is per render frame and gets missed when a press lands in a
		// hitched or just-started frame, which ate jump presses (the "sometimes can't jump"
		// report). Our own previous-state comparison cannot miss one.
		bool spaceDown = Input.IsKeyDown(Key.Space) || Gamepad.IsDown(GamepadButton.A);
		_jumpLatch |= spaceDown && !_spaceWasDown;
		_spaceWasDown = spaceDown;
		bool spinDown = Input.IsKeyDown(Key.E) || Input.IsMouseDown(MouseButton.Left) || Gamepad.IsDown(GamepadButton.X);
		_spinLatch |= spinDown && !_spinWasDown;
		_spinWasDown = spinDown;
		bool crouchDown = Input.IsKeyDown(Key.C) || Input.IsKeyDown(Key.LeftCtrl) || Input.IsMouseDown(MouseButton.Right) || Gamepad.IsDown(GamepadButton.B);
		_crouchLatch |= crouchDown && !_crouchWasDown;
		_crouchWasDown = crouchDown;

		if (_control)
		{
			_accumulator = Math.Min(_accumulator + deltaTime, 0.1f);
			while (_accumulator >= Tick)
			{
				_accumulator -= Tick;
				Step(Tick);
			}
			Vector3 velocity = new(_horizontal.X, TrackHeight(), _horizontal.Z);
			if (!Airborne)
			{
				// Keep the measured horizontal speed and follow a DESCENDING ground plane, so
				// running down a walkable slope stays on it instead of launching off. Uphill is left
				// to the contact solve: following the rising side of a sand ripple carried its climb
				// past the crest and launched him over every ripple.
				Vector3 normal = CharacterController.GetGroundNormal(Self);
				if (normal.Y > 0.1f)
				{
					velocity.Y += MathF.Min(0.0f, -(normal.X * velocity.X + normal.Z * velocity.Z) / normal.Y);
				}
				velocity -= normal * GroundPress;
			}
			CharacterController.SetVelocity(Self, velocity);
		}
		else
		{
			_horizontal = Vector3.Zero;
			_vy = 0.0f;
			CharacterController.SetVelocity(Self, Vector3.Zero);
		}
		PlaceModel(deltaTime);
		PlaceCamera();
	}

	// Deaths play the OnDefaultDeath behaviour-script sequence (act_CRASH.states.json): a081 is the
	// wide-eyed panic (blend 0.5), a082 the look-down realization, a006 + a007 keel over backwards
	// (one-shot, cut), a083 lies flat. Per kind, the original varies which of those run:
	//   Generic / Drown - the full panic sequence (drowning runs it too, while he sinks).
	//   Explode - no panic, he is simply blasted over: a006 + a007 + a083.
	//   Fall - a short panic then straight over (OnFallingDeath, state 11, has no clip table of its
	//   own in the extracted states, so the sequence is the generic one shortened).
	private static readonly Dictionary<DeathKind, (string Clip, float Time)[]> DeathSequences = new()
	{
		[DeathKind.Generic] = new[] { ("a081", 1.2f), ("a082", 0.8f), ("a006", 0.5f), ("a007", 0.5f), ("a083", 1.0f) },
		[DeathKind.Drown] = new[] { ("a081", 1.2f), ("a082", 0.8f), ("a006", 0.5f), ("a007", 0.5f), ("a083", 1.0f) },
		[DeathKind.Explode] = new[] { ("a006", 0.5f), ("a007", 0.5f), ("a083", 1.0f) },
		[DeathKind.Fall] = new[] { ("a081", 1.2f), ("a006", 0.5f), ("a007", 0.5f), ("a083", 1.0f) },
	};

	/// <summary>Kills Crash: takes control away, keeps the model visible and plays the original
	/// death clip sequence for the kind. Returns the seconds until the level should respawn him.</summary>
	public float Die(DeathKind kind)
	{
		if (_dead)
		{
			return 0.0f;
		}
		_dead = true;
		_deathSeq = DeathSequences[kind];
		_deathIndex = 0;
		_deathTimer = _deathSeq[0].Time;
		_horizontal = Vector3.Zero;
		_vy = 0.0f;
		_spinTime = 0.0f;
		Animation.SetLayerClip(_model, -1);
		CharacterController.SetVelocity(Self, Vector3.Zero);
		Play(_deathSeq[0].Clip, false);
		float total = 0.0f;
		foreach (var entry in _deathSeq)
		{
			total += entry.Time;
		}
		// One extra second of lying there, as in the original, before the level respawns him.
		return total + 1.0f;
	}

	/// <summary>A hit that Aku Aku absorbs: the original's recoil (a084, blend 0.2) and a short
	/// push away from the hazard that the ordinary ground braking eats.</summary>
	public void Hurt(Vector3 from)
	{
		if (_dead || _hurtLeft > 0.0f)
		{
			return;
		}
		Vector3 away = Self.Position - from;
		away.Y = 0.0f;
		_horizontal = away.LengthSquared() > 1e-6f
			? Vector3.Normalize(away) * 3.0f
			: -FacingDir(_facing) * 3.0f;
		_hurtLeft = 0.4f;
		Play("a084", false);
		_landClip = "a084";
		_oneShotLeft = 0.4f;
	}

	// Death: control is gone, the model stays put (the original freezes him in place), and the
	// clip sequence for the kind plays out. The level respawns him after the seconds Die returned.
	private void StepDeath(float deltaTime)
	{
		_deathTimer -= deltaTime;
		if (_deathTimer <= 0.0f && _deathIndex + 1 < _deathSeq.Length)
		{
			_deathIndex++;
			_deathTimer = _deathSeq[_deathIndex].Time;
			Play(_deathSeq[_deathIndex].Clip, false);
		}
		PlaceModel(0.0f);
		PlaceCamera();
	}

	/// <summary>Launch upward at <paramref name="speed"/>, keeping horizontal motion: a crate or
	/// spring bounce. Works airborne.</summary>
	public void Bounce(float speed)
	{
		_vy = speed;
		_arc = Arc.Bounce;
		_doubleJumped = false;
		Enter(State.Air);
		Play("a019", false);
	}

	public void Respawn(Vector3 feet, float facing)
	{
		_dead = false;
		_hurtLeft = 0.0f;
		Animation.SetLayerClip(_model, -1);
		Self.Position = feet;
		_horizontal = Vector3.Zero;
		_vy = 0.0f;
		_spinTime = 0.0f;
		_facing = facing;
		_modelYaw = facing;
		_cameraYaw = facing;
		_moveDir = FacingDir(facing);
		Enter(State.Ground);
		CharacterController.SetVelocity(Self, Vector3.Zero);
		PlaceModel(0.0f);
		PlaceCamera();
	}

	public void SetControl(bool enabled)
	{
		_control = enabled;
		_model.SetActive(enabled);
	}

	private void Step(float dt)
	{
		bool jump = _jumpLatch;
		bool spin = _spinLatch;
		bool crouchPressed = _crouchLatch;
		_jumpLatch = _spinLatch = _crouchLatch = false;
		bool crouchHeld = Input.IsKeyDown(Key.C) || Input.IsKeyDown(Key.LeftCtrl) || Input.IsMouseDown(MouseButton.Right) || Gamepad.IsDown(GamepadButton.B);

		var (stickDir, stick) = StickInput();
		bool moving = stick >= StickDeadZone;
		bool grounded = CharacterController.IsGrounded(Self);
		_stateTime += dt;

		if (grounded)
		{
			_groundGrace = GroundGraceTicks;
		}
		else if (_groundGrace > 0)
		{
			_groundGrace--;
		}

		_spinCooldown -= dt;
		if (_spinTime > 0.0f)
		{
			_spinTime -= dt;
			if (_spinTime <= 0.0f)
			{
				_spinCooldown = _spinDelay;
				_oneShotLeft = 0.2f;
				_landClip = moving ? "a015" : "a016";
				Animation.SetLayerClip(_model, -1);
			}
		}
		else if (spin && _spinCooldown <= 0.0f && _state is State.Ground or State.Air)
		{
			_spinTime = _spinLength;
			// a046 only animates the root joint; the original keeps the locomotion clip posing the
			// body and layers the spin on top. Set once here (calling it again resets its clock);
			// Play() re-applies it whenever the base clip changes mid-spin.
			_spinBase = _clip is ("a015" or "a016" or "a084") ? "a008" : _clip;
			if (Animation.Find(_model, "a046") >= 0)
			{
				Animation.SetLayerClip(_model, Animation.Find(_model, "a046"));
			}
		}

		switch (_state)
		{
			case State.Ground:
			{
				// Jump first: IsGrounded flickering false for a tick must not eat the press.
				if (jump && (grounded || _groundGrace > 0))
				{
					StartJump(_horizontal.Length() > 1.0f ? "a020" : "a019");
					break;
				}
				if (!grounded)
				{
					if (_groundGrace <= 0)
					{
						_arc = Arc.Fall;
						_vy = 0.0f;
						_doubleJumped = false;
						Enter(State.Air);
						break;
					}
				}
				else if (_hurtLeft > 0.0f)
				{
					_horizontal = Brake(_horizontal, BrakeRate * dt);
				}
				else if (moving)
				{
					_moveDir = stickDir;
					_horizontal = stickDir * (stick >= StickRun ? _runSpeed : _walkSpeed);
				}
				else
				{
					_horizontal = Brake(_horizontal, BrakeRate * dt);
				}
				_vy = 0.0f;
				if (crouchPressed && !IsSpinning)
				{
					Enter(_horizontal.Length() >= _runSpeed - 0.5f ? State.Slide : State.Crouch);
				}
				break;
			}

			case State.Crouch:
				_horizontal = moving && _hurtLeft <= 0.0f ? stickDir * _crawlSpeed : _hurtLeft > 0.0f ? Brake(_horizontal, BrakeRate * dt) : Vector3.Zero;
				if (moving && _hurtLeft <= 0.0f)
				{
					_moveDir = stickDir;
				}
				_vy = 0.0f;
				if (jump)
				{
					StartJump("a019");
				}
				else if (!grounded && _groundGrace <= 0)
				{
					_arc = Arc.Fall;
					Enter(State.Air);
				}
				else if (!crouchHeld && _stateTime > 0.1f)
				{
					Enter(State.Ground);
				}
				break;

			case State.Slide:
				if (moving)
				{
					_moveDir = stickDir;
				}
				float speed = SlideSpeedAt(_stateTime);
				_horizontal = _moveDir * speed;
				_vy = 0.0f;
				if (jump && _stateTime < SlideDelay + SlideHold + SlideStop)
				{
					_vy = _slideJumpHeight - _slideJumpGravity * Tick;
					_arc = Arc.SlideJump;
					_doubleJumped = true;
					Enter(State.Air);
					Play("a048", false);
				}
				else if (!grounded)
				{
					_arc = Arc.Fall;
					Enter(State.Air);
				}
				else if (_stateTime >= SlideDelay + SlideHold + SlideStop + SlideCrouch)
				{
					Enter(crouchHeld ? State.Crouch : State.Ground);
				}
				break;

			case State.Air:
				if (_hurtLeft <= 0.0f)
				{
					AirHorizontal(stickDir, moving, dt);
				}
				// Land before anything else: when ground contact and the press arrive on the same
				// tick, the press is a ground jump. Checking it after the double jump spent the
				// press as a double jump launched off the floor (the "no jump from the ground" /
				// "no double jump" reports). Gravity is folded in so the landing tick matches the
				// old post-gravity check. Only launches wait a tick: on the take-off tick they still
				// touch the floor, but a fall that finds ground again lands at once.
				if (grounded && _vy - AirGravityNow() * dt <= 0.0f && (_arc == Arc.Fall || _stateTime > Tick))
				{
					bool jumpNow = jump || _jumpBuffer > 0;
					Land(moving ? "a030" : "a029", 0.2f);
					if (jumpNow)
					{
						StartJump(_horizontal.Length() > 1.0f ? "a020" : "a019");
					}
					break;
				}
				if (_jumpBuffer > 0)
				{
					_jumpBuffer--;
				}
				if (jump && !_doubleJumped && _arc is Arc.Jump or Arc.Fall or Arc.Bounce)
				{
					_doubleJumped = true;
					_vy = _doubleJumpHeight;
					_arc = Arc.DoubleJump;
					Play("a021", false);
				}
				else if (jump)
				{
					// Double jump spent: hold the press for the ground contact the physics
					// reports up to two ticks late (the mirror of the ground-loss grace).
					_jumpBuffer = GroundGraceTicks;
				}
				else if (crouchPressed && _arc != Arc.SlideJump)
				{
					Enter(State.SlamHang);
					break;
				}
				_vy -= AirGravityNow() * dt;
				break;

			case State.SlamHang:
				_horizontal = Vector3.Zero;
				_vy = _stateTime < SlamStill ? 0.0f : SlamRise;
				if (_stateTime >= _slamHang)
				{
					_vy = SlamDropStart;
					Enter(State.SlamDrop);
				}
				break;

			case State.SlamDrop:
				_horizontal = Vector3.Zero;
				_vy -= _slamGravity * dt;
				if (grounded && _stateTime > Tick)
				{
					_vy = 0.0f;
					Enter(State.SlamLand);
				}
				break;

			case State.SlamLand:
				_horizontal = Vector3.Zero;
				_vy = 0.0f;
				if (_stateTime >= SlamLandLock)
				{
					Enter(State.Ground);
				}
				break;
		}

		// Ceiling: while rising, the body trails the height the jump should have reached by at most
		// what physics has not integrated yet (one frame plus one physics step of rise) - unless
		// something above stopped it, when the gap grows without bound. Frame-rate independent,
		// unlike comparing positions between our ticks (two ticks can run with no physics step
		// between them below 50 fps, which cut every jump to ~0.4 m once the game slowed down).
		if (_state == State.Air && _vy > 0.0f)
		{
			float slack = 0.25f + _vy * (Time.DeltaTime + 1.0f / 60.0f);
			if (_airY - Self.Position.Y > slack)
			{
				_vy = 0.0f;
				_airY = Self.Position.Y;
			}
		}

		if (Airborne)
		{
			_airY += _vy * dt;
		}
		else
		{
			_airY = Self.Position.Y;
		}

		Animate(dt, moving, stick);
	}

	private bool Airborne => _state is State.Air or State.SlamHang or State.SlamDrop;

	// The logic steps at the game's 50 Hz but physics integrates at 60 Hz, so each 50 Hz velocity is
	// held for one or two physics steps depending on phase, and a jump's apex wandered by +-0.1 m
	// between otherwise identical jumps. Airborne, the script keeps the height the game's own
	// integration gives (_airY, advanced by the same velocity each tick) and steers onto it.
	private float TrackHeight()
	{
		if (!Airborne)
		{
			return _vy;
		}
		float target = _airY + _vy * _accumulator;
		return _vy + Math.Clamp(target - Self.Position.Y, -0.5f, 0.5f) * HeightGain;
	}

	// The game applies the take-off frame's gravity before moving: the first rise is 12.25, not 13.
	private void StartJump(string clip)
	{
		_vy = _jumpHeight - _jumpRiseGravity * Tick;
		_arc = Arc.Jump;
		_doubleJumped = false;
		Enter(State.Air);
		Play(clip, false);
	}

	private void Land(string clip, float lock_)
	{
		_vy = 0.0f;
		Enter(State.Ground);
		_landClip = clip;
		_oneShotLeft = lock_;
	}

	private void Enter(State state)
	{
		_state = state;
		_stateTime = 0.0f;
		if (state != State.Air)
		{
			_jumpBuffer = 0;
		}
		switch (state)
		{
			case State.Slide:
				Play("a036", false);
				break;
			case State.Crouch:
				Play("a032", false);
				break;
			case State.SlamHang:
				Play("a023", false);
				break;
			case State.SlamDrop:
				Play("a024", true);
				break;
			case State.SlamLand:
				Play("a026", false);
				break;
		}
	}

	private float SlideSpeedAt(float t)
	{
		if (t < SlideDelay)
		{
			return _runSpeed;
		}
		t -= SlideDelay;
		if (t < SlideHold)
		{
			return _slideSpeed;
		}
		t -= SlideHold;
		if (t < SlideStop)
		{
			return _slideSpeed * (1.0f - t / SlideStop);
		}
		var (_, stick) = StickInput();
		return stick >= StickDeadZone ? _crawlSpeed : 0.0f;
	}

	private void AirHorizontal(Vector3 stickDir, bool moving, float dt)
	{
		if (_arc == Arc.SlideJump)
		{
			if (moving)
			{
				_moveDir = stickDir;
			}
			float speed = _horizontal.Length();
			speed = speed > _slideJumpSpeed ? Math.Max(_slideJumpSpeed, speed - AirAccel * dt) : _slideJumpSpeed;
			_horizontal = _moveDir * speed;
			return;
		}
		float cap = _arc == Arc.Fall ? _edgeSpeed : _jumpAirSpeed;
		Vector3 target = moving ? stickDir * cap : Vector3.Zero;
		if (moving)
		{
			_moveDir = stickDir;
		}
		_horizontal = MoveTowards(_horizontal, target, AirAccel * dt);
	}

	private float AirGravityNow() => _arc switch
	{
		Arc.Jump => _vy > 0.0f ? _jumpRiseGravity : _airGravity,
		Arc.DoubleJump => _vy > 0.0f ? _doubleJumpGravity : _airGravity,
		Arc.SlideJump => _vy > SlideJumpFallAt ? _slideJumpGravity : _airGravity,
		_ => _airGravity,
	};

	private void Animate(float dt, bool moving, float stick)
	{
		if (_state is State.SlamHang or State.SlamDrop or State.SlamLand or State.Slide)
		{
			if (_state == State.Slide && _stateTime >= SlideDelay + SlideHold + SlideStop)
			{
				Play("a032", false);
			}
			return;
		}
		// During a spin the base locomotion/idle clip keeps playing normally (the a046 spin is a
		// root-only layer set at spin start); Play() re-applies the layer if the base changes.
		if (_state == State.Crouch)
		{
			if (_horizontal.LengthSquared() > 0.01f)
			{
				Play("a034", true);
			}
			else if (_clip == "a034")
			{
				Play("a032", false);
			}
			return;
		}
		if (_state == State.Air)
		{
			// Jump clips play out; the fall pose takes over once one has finished.
			if (_arc == Arc.Fall || ClipFinished())
			{
				Play("a027", true);
			}
			return;
		}

		if (_oneShotLeft > 0.0f)
		{
			_oneShotLeft -= dt;
			if (!moving || _hurtLeft > 0.0f)
			{
				Play(_landClip, false);
				return;
			}
		}
		if (_horizontal.LengthSquared() > 0.01f && moving)
		{
			_idleTime = 0.0f;
			// OnWalk plays a010 (a041 only while holding the multi-tool); OnRun plays a011. Both
			// switch to the push clips (a044 walk, a045 run) while IsPushingObject holds - set by
			// TwinsanityMechanics while he walks into a pushable.
			Play(stick >= StickRun ? (Pushing ? "a045" : "a011") : (Pushing ? "a044" : "a010"), true);
			return;
		}
		_idleTime += dt;
		if (_idleTime >= IdleFidgetAfter)
		{
			_idleTime = 0.0f;
			string[] fidgets = { "a094", "a095", "a096", "a097", "a098", "a099", "a100", "a101", "a102", "a103" };
			Play(fidgets[_random.Next(fidgets.Length)], false);
			return;
		}
		if (_clip is not ("a094" or "a095" or "a096" or "a097" or "a098" or "a099" or "a100" or "a101" or "a102" or "a103") || ClipFinished())
		{
			Play("a008", true);
		}
	}

	private float _clipStarted;
	private float _clipDuration;

	private bool ClipFinished() => Time.TotalTime - _clipStarted >= _clipDuration;

	// Blend-in time per clip: the second DoAnim argument in the behaviour script that plays it
	// (act_CRASH.states.json). 0 is an instant cut; the spin, slam and crouch cut in the original too.
	private static readonly Dictionary<string, float> BlendIn = new()
	{
		["a008"] = 0.2f, ["a010"] = 0.2f, ["a011"] = 0.2f,
		["a019"] = 0.1f, ["a020"] = 0.1f, ["a021"] = 0.1f, ["a048"] = 0.1f,
		["a027"] = 0.1f, ["a029"] = 0.1f, ["a030"] = 0.1f,
		["a015"] = 0.0f, ["a016"] = 0.0f, ["a046"] = 0.0f,
		["a023"] = 0.2f, ["a024"] = 0.0f, ["a026"] = 0.0f,
		["a032"] = 0.0f, ["a034"] = 0.0f, ["a036"] = 0.1f,
	};
	private const float DefaultBlendIn = 0.2f; // the idle fidgets (a094-a103)

	private void Play(string name, bool loop)
	{
		if (name == _clip)
		{
			return;
		}
		if (!_clips.TryGetValue(name, out int index))
		{
			index = Animation.Find(_model, name);
			_clips[name] = index;
		}
		if (index < 0)
		{
			return;
		}
		_clip = name;
		Animation.CrossFade(_model, index, BlendIn.TryGetValue(name, out float blend) ? blend : DefaultBlendIn);
		// A CrossFade would clear the spin layer; re-apply it over the new base clip.
		if (IsSpinning && Animation.Find(_model, "a046") >= 0)
		{
			Animation.SetLayerClip(_model, Animation.Find(_model, "a046"));
		}
		SetLooping(_model, loop);
		_clipStarted = Time.TotalTime;
		_clipDuration = Animation.ClipDuration(_model);
	}

	private static void SetLooping(Entity entity, bool loop)
	{
		ComponentAccess skinned = entity.Component("Skinned Mesh");
		if (skinned.Exists)
		{
			skinned.SetBool("looping", loop);
		}
		for (int i = 0; i < entity.ChildCount; i++)
		{
			SetLooping(entity.GetChild(i), loop);
		}
	}

	private (Vector3 Dir, float Amount) StickInput()
	{
		Vector3 forward = Flat(Camera.GetForward(_camera));
		Vector3 right = Flat(Camera.GetRight(_camera));
		Vector2 pad = Gamepad.LeftStick;
		float x = Input.GetAxisRaw(Key.A, Key.D) + Input.GetAxisRaw(Key.Left, Key.Right) + pad.X;
		float z = Input.GetAxisRaw(Key.S, Key.W) + Input.GetAxisRaw(Key.Down, Key.Up) + pad.Y;
		// The game reads deflection per axis: a half-way diagonal walks where a full push runs.
		float amount = Math.Min(Math.Max(Math.Abs(x), Math.Abs(z)), 1.0f);
		Vector3 input = forward * z + right * x;
		return input.LengthSquared() > 1e-6f ? (Vector3.Normalize(input), amount) : (Vector3.Zero, 0.0f);
	}

	private void Look(float deltaTime)
	{
		Vector2 mouse = Input.MouseDelta;
		Vector2 stick = Gamepad.RightStick;
		_cameraYaw -= mouse.X * MouseSensitivity + stick.X * StickTurnSpeed * deltaTime;
		CameraPitch = Math.Clamp(CameraPitch + mouse.Y * MouseSensitivity - stick.Y * StickTurnSpeed * 0.5f * deltaTime, 0.0f, 60.0f);
	}

	private void PlaceModel(float deltaTime)
	{
		if (_moveDir.LengthSquared() > 1e-6f)
		{
			_facing = MathF.Atan2(-_moveDir.X, -_moveDir.Z) * (180.0f / MathF.PI);
		}
		float delta = Wrap(_facing - _modelYaw);
		float step = deltaTime > 0.0f ? ModelTurnRate * deltaTime : 360.0f;
		_modelYaw += Math.Clamp(delta, -step, step);
		_model.Position = Self.Position;
		_model.EulerDegrees = new Vector3(0.0f, _modelYaw + ModelYawOffset, 0.0f);
	}

	// shortcut: no camera collision, so walls can come between camera and Crash; add a
	// Physics.SphereCast from target to eye if that gets in the way.
	private void PlaceCamera()
	{
		Camera.SetTarget(_camera, Self.Position + new Vector3(0.0f, CameraTargetHeight, 0.0f));
		Camera.SetOrbital(_camera, _cameraYaw, CameraPitch, CameraDistance);
	}

	private static Vector3 FacingDir(float yawDegrees)
	{
		float r = yawDegrees * (MathF.PI / 180.0f);
		return new Vector3(-MathF.Sin(r), 0.0f, -MathF.Cos(r));
	}

	private static Vector3 Flat(Vector3 v)
	{
		var flat = new Vector3(v.X, 0.0f, v.Z);
		float length = flat.Length();
		return length > 1e-4f ? flat / length : Vector3.Zero;
	}

	private static float Wrap(float degrees)
	{
		degrees %= 360.0f;
		return degrees > 180.0f ? degrees - 360.0f : degrees < -180.0f ? degrees + 360.0f : degrees;
	}

	private static Vector3 Brake(Vector3 v, float amount)
	{
		float speed = v.Length();
		return speed <= amount ? Vector3.Zero : v * ((speed - amount) / speed);
	}

	private static Vector3 MoveTowards(Vector3 current, Vector3 target, float maxDelta)
	{
		Vector3 toTarget = target - current;
		float distance = toTarget.Length();
		return distance <= maxDelta || distance < 1e-5f ? target : current + toTarget / distance * maxDelta;
	}
}

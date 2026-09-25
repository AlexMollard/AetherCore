using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Crash: third-person run, jump and spin on the scene's Character Controller, with an orbit
/// camera behind him. The level's rules (crates, wumpa, death) live in TwinsanityLevel, which
/// reads IsSpinning/Velocity and calls Bounce/Respawn/SetControl.
///
/// The model is a separate root entity, not a child: Entity transform writes move a subtree by a
/// world delta, and the Character Controller writes this entity's transform on its own schedule,
/// so a child would be dragged twice. The model is placed explicitly every frame instead.
///
/// Facing uses the camera's yaw convention (forward = (-sin yaw, 0, -cos yaw), as
/// aether_camera_get_yaw), so camera and body share one angle. Crash's glTF faces +Z.
/// </summary>
public sealed class CrashPlayer : EntityScript
{
	public string ModelPath = "project://assets/models/objects/act_CRASH/act_CRASH_0.gltf";
	public float ModelYawOffset = 180.0f;

	public float RunSpeed = 8.0f;
	public float GroundAcceleration = 60.0f;
	public float GroundDeceleration = 80.0f;
	public float AirControl = 0.6f;
	public float JumpSpeed = 10.0f;
	public float TurnRate = 900.0f;

	public float SpinDuration = 0.45f;
	public float SpinCooldown = 0.2f;

	public float CameraDistance = 7.5f;
	public float CameraPitch = 20.0f;
	public float CameraTargetHeight = 1.4f;
	public float MouseSensitivity = 0.15f;
	public float StickTurnSpeed = 160.0f;

	public bool IsSpinning => _spinTime > 0.0f;
	public Vector3 Velocity => CharacterController.GetVelocity(Self);
	public bool IsGrounded => CharacterController.IsGrounded(Self);

	private Entity _model;
	private Entity _camera;
	private float _facing;
	private float _cameraYaw;
	private Vector3 _horizontalVelocity;
	private float _spinTime;
	private float _spinCooldown;
	private float _spinAngle;
	private bool _control = true;

	public override void OnAttach()
	{
		_model = World.Create();
		_model.Name = "Crash Model";
		_model.AddTransform();
		_model.Position = Self.Position;
		_model.LoadModel(ModelPath);

		_facing = Self.EulerDegrees.Y;
		_cameraYaw = _facing;
		_camera = Camera.CreateOrbit(Self.Position + new Vector3(0.0f, 3.0f, 6.0f), Self.Position, 60.0f);
		_camera.Name = "Crash Camera";
		Camera.SetMain(_camera);
		PlaceCamera();
		PlaceModel();
	}

	public override void OnDetach()
	{
		Input.CursorLockRequested = false;
	}

	public override void OnUpdate(float deltaTime)
	{
		Input.CursorLockRequested = true;
		Look(deltaTime);
		if (_control)
		{
			Move(deltaTime);
		}
		else
		{
			_horizontalVelocity = Vector3.Zero;
			CharacterController.Move(Self, Vector3.Zero, 0.0f);
		}
		PlaceModel();
		PlaceCamera();
	}

	/// <summary>Launch upward at <paramref name="speed"/>, keeping horizontal motion: a crate or
	/// spring bounce. Works airborne, unlike Jump.</summary>
	public void Bounce(float speed)
	{
		Vector3 v = CharacterController.GetVelocity(Self);
		CharacterController.SetVelocity(Self, new Vector3(v.X, speed, v.Z));
	}

	public void Respawn(Vector3 feet, float facing)
	{
		Self.Position = feet;
		_horizontalVelocity = Vector3.Zero;
		_spinTime = 0.0f;
		_facing = facing;
		_cameraYaw = facing;
		CharacterController.Move(Self, Vector3.Zero, 0.0f);
		PlaceModel();
		PlaceCamera();
	}

	public void SetControl(bool enabled)
	{
		_control = enabled;
		_model.SetActive(enabled);
	}

	public float Facing => _facing;

	private void Look(float deltaTime)
	{
		Vector2 mouse = Input.MouseDelta;
		Vector2 stick = Gamepad.RightStick;
		_cameraYaw -= mouse.X * MouseSensitivity + stick.X * StickTurnSpeed * deltaTime;
		CameraPitch = Math.Clamp(CameraPitch + mouse.Y * MouseSensitivity - stick.Y * StickTurnSpeed * 0.5f * deltaTime, 0.0f, 60.0f);
	}

	private void Move(float deltaTime)
	{
		Vector3 forward = Flat(Camera.GetForward(_camera));
		Vector3 right = Flat(Camera.GetRight(_camera));

		Vector2 stick = Gamepad.LeftStick;
		float x = Input.GetAxisRaw(Key.A, Key.D) + Input.GetAxisRaw(Key.Left, Key.Right) + stick.X;
		float z = Input.GetAxisRaw(Key.S, Key.W) + Input.GetAxisRaw(Key.Down, Key.Up) + stick.Y;
		Vector3 input = forward * z + right * x;
		float amount = Math.Min(input.Length(), 1.0f);
		Vector3 direction = amount > 0.01f ? Vector3.Normalize(input) : Vector3.Zero;

		bool grounded = CharacterController.IsGrounded(Self);
		float accel = grounded ? (amount > 0.01f ? GroundAcceleration : GroundDeceleration) : GroundAcceleration * AirControl;
		_horizontalVelocity = MoveTowards(_horizontalVelocity, direction * RunSpeed * amount, accel * deltaTime);
		float speed = _horizontalVelocity.Length();
		CharacterController.Move(Self, speed > 1e-4f ? _horizontalVelocity / speed : Vector3.Zero, speed);

		if (amount > 0.01f)
		{
			float target = MathF.Atan2(-direction.X, -direction.Z) * (180.0f / MathF.PI);
			float delta = Wrap(target - _facing);
			_facing += Math.Clamp(delta, -TurnRate * deltaTime, TurnRate * deltaTime);
		}

		if (grounded && (Input.IsKeyPressed(Key.Space) || Gamepad.IsPressed(GamepadButton.A)))
		{
			CharacterController.Jump(Self, JumpSpeed);
		}

		_spinCooldown -= deltaTime;
		if (_spinTime > 0.0f)
		{
			_spinTime -= deltaTime;
			_spinAngle += 360.0f * 2.0f / SpinDuration * deltaTime;
			if (_spinTime <= 0.0f)
			{
				_spinCooldown = SpinCooldown;
				_spinAngle = 0.0f;
			}
		}
		else if (_spinCooldown <= 0.0f && (Input.IsKeyPressed(Key.E) || Input.IsMousePressed(MouseButton.Left) || Gamepad.IsPressed(GamepadButton.X)))
		{
			_spinTime = SpinDuration;
		}
	}

	private void PlaceModel()
	{
		_model.Position = Self.Position;
		_model.EulerDegrees = new Vector3(0.0f, _facing + ModelYawOffset + _spinAngle, 0.0f);
	}

	// shortcut: no camera collision, so walls can come between camera and Crash; add a
	// Physics.SphereCast from target to eye if that gets in the way.
	private void PlaceCamera()
	{
		Camera.SetTarget(_camera, Self.Position + new Vector3(0.0f, CameraTargetHeight, 0.0f));
		Camera.SetOrbital(_camera, _cameraYaw, CameraPitch, CameraDistance);
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

	private static Vector3 MoveTowards(Vector3 current, Vector3 target, float maxDelta)
	{
		Vector3 toTarget = target - current;
		float distance = toTarget.Length();
		return distance <= maxDelta || distance < 1e-5f ? target : current + toTarget / distance * maxDelta;
	}
}

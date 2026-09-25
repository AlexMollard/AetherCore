using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using Twinsanity;

namespace TwExtract
{
	// Game object skeletal animations -> glTF clips on the object's joint nodes.
	//
	// Evaluation follows the Twinsanity editor's AnimationController / AnimationViewer, which is what the
	// community verified against the game. Per joint, TransformationChoice picks, channel by channel in the
	// order tx ty tz rx ry rz sx sy sz, a per-frame value from the frame's AnimatedTransform (bit clear) or a
	// constant from StaticTransforms (bit set). Rotations are Euler angles applied X, then Y, then Z; flag bit
	// 12 composes the joint's "additional rotation" (Matrix[4]) after it, and bit 13 cancels the parent's
	// animated scale. Everything is computed in the game's space and mirrored in X at the end, the same
	// Space.Mirror the skeleton and meshes go through.
	static class AnimExport
	{
		public const float Fps = 25f;

		// endLocals, when given, receives per clip each joint's local matrix (row-vector S*R*T, mirrored)
		// on the last frame under the clip's name and on the first frame under "<name>@0" - the poses a
		// play-once prop rests in and holds; null for a joint the clip leaves alone.
		public static int Add(Gltf g, GraphicsInfo.Joint[] joints, int[] nodes, IEnumerable<(string Name, Animation Anim)> clips, Dictionary<string, Matrix4x4?[]> endLocals = null)
		{
			int added = 0;
			var byIndex = new Dictionary<uint, int>();
			for (int i = 0; i < joints.Length; i++)
			{
				byIndex[joints[i].JointIndex] = i;
			}
			// Parents before children, so a child can read its parent's animated scale.
			var order = Enumerable.Range(0, joints.Length).OrderBy(i => Depth(joints, byIndex, i)).ToArray();
			foreach (var (name, anim) in clips)
			{
				if (anim.TotalFrames == 0 || anim.AnimatedTransforms.Count < anim.TotalFrames)
				{
					continue;
				}
				int frames = anim.TotalFrames;
				var times = Enumerable.Range(0, frames).Select(f => f / Fps).ToArray();
				var t = new float[joints.Length][];
				var r = new float[joints.Length][];
				var s = new float[joints.Length][];
				for (int i = 0; i < joints.Length; i++)
				{
					t[i] = new float[frames * 3];
					r[i] = new float[frames * 4];
					s[i] = new float[frames * 3];
				}
				var rawScale = new Vector3[joints.Length];
				var tracks = new List<(int Node, float[] T, float[] R, float[] S)>();
				for (int f = 0; f < frames; f++)
				{
					foreach (int i in order)
					{
						var joint = joints[i];
						if (joint.JointIndex >= anim.JointsSettings.Count)
						{
							// This clip does not animate this joint (partial clips like the spin animate the
							// root only). Emit no channels for it so the engine layers the clip over whatever
							// the previous clip posed - the bind pose would give a T-pose.
							t[i] = null;
							r[i] = null;
							s[i] = null;
							rawScale[i] = Vector3.One;
							continue;
						}
						var js = anim.JointsSettings[(int)joint.JointIndex];
						var (tr, rot, scale) = Evaluate(anim, js, f);
						if ((js.Flags >> 12 & 1) != 0)
						{
							var add = joint.Matrix[4];
							rot = Quaternion.Normalize(new Quaternion(add.X, add.Y, add.Z, add.W) * rot);
						}
						rawScale[i] = scale;
						var local = scale;
						bool hasParent = byIndex.TryGetValue(joint.ParentJointIndex, out int parent) && parent != i;
						if ((js.Flags >> 13 & 1) != 0 && hasParent)
						{
							var ps = rawScale[parent];
							local = new Vector3(Div(scale.X, ps.X), Div(scale.Y, ps.Y), Div(scale.Z, ps.Z));
						}
						var mt = Space.Mirror(tr);
						var mr = Space.Mirror(rot);
						t[i][f * 3] = mt.X; t[i][f * 3 + 1] = mt.Y; t[i][f * 3 + 2] = mt.Z;
						r[i][f * 4] = mr.X; r[i][f * 4 + 1] = mr.Y; r[i][f * 4 + 2] = mr.Z; r[i][f * 4 + 3] = mr.W;
						s[i][f * 3] = local.X; s[i][f * 3 + 1] = local.Y; s[i][f * 3 + 2] = local.Z;
						if (f == 0)
						{
							tracks.Add((nodes[i], t[i], r[i], s[i]));
						}
					}
				}
				// glTF slerps the shortest way; keep consecutive keys in one hemisphere so it does.
				foreach (var q in r)
				{
					if (q == null)
					{
						continue;
					}
					for (int f = 1; f < frames; f++)
					{
						float dot = q[f * 4] * q[f * 4 - 4] + q[f * 4 + 1] * q[f * 4 - 3] + q[f * 4 + 2] * q[f * 4 - 2] + q[f * 4 + 3] * q[f * 4 - 1];
						if (dot < 0)
						{
							for (int c = 0; c < 4; c++)
							{
								q[f * 4 + c] = -q[f * 4 + c];
							}
						}
					}
				}
				if (endLocals != null)
				{
					foreach (int l in new[] { frames - 1, 0 })
					{
						var pose = new Matrix4x4?[joints.Length];
						for (int i = 0; i < joints.Length; i++)
						{
							if (t[i] == null)
							{
								continue;
							}
							var q = new Quaternion(r[i][l * 4], r[i][l * 4 + 1], r[i][l * 4 + 2], r[i][l * 4 + 3]);
							pose[i] = Matrix4x4.CreateScale(s[i][l * 3], s[i][l * 3 + 1], s[i][l * 3 + 2]) * Matrix4x4.CreateFromQuaternion(q)
								* Matrix4x4.CreateTranslation(t[i][l * 3], t[i][l * 3 + 1], t[i][l * 3 + 2]);
						}
						endLocals[l == 0 ? name + "@0" : name] = pose;
					}
				}
				g.AddAnimation(name, times, tracks);
				added++;
			}
			return added;
		}

		static float Div(float a, float b) => Math.Abs(b) > 1e-6f ? a / b : a;

		static int Depth(GraphicsInfo.Joint[] joints, Dictionary<uint, int> byIndex, int i)
		{
			int depth = 0;
			while (depth < joints.Length && byIndex.TryGetValue(joints[i].ParentJointIndex, out int p) && p != i)
			{
				i = p;
				depth++;
			}
			return depth;
		}

		const int DoAnim = 9; // DefaultEnums.CommandID.DoAnim

		// A character's behaviour slots (DefaultEnums.CharacterGameObjectScriptOrder: OnIdle, OnRun, OnSpin...)
		// -> the animation slots its script plays, in script order. A DoAnim command's last argument packs up to
		// four slot bytes (0xFF = none; several means the game picks one at random) and its second is the blend
		// time as a float (tiny values are flags, not times). Clip names match Add's "aNNN".
		public static Dictionary<string, object> CharacterStates(GameObject obj, Dictionary<uint, Script> scripts)
		{
			var states = new Dictionary<string, object>();
			var slotNames = Enum.GetNames(typeof(DefaultEnums.CharacterGameObjectScriptOrder));
			for (int slot = 0; slot < obj.Scripts.Count && slot < slotNames.Length; slot++)
			{
				if (!scripts.TryGetValue(obj.Scripts[slot], out var script))
				{
					continue;
				}
				var mains = new List<Script.MainScript>();
				if (script.Main != null)
				{
					mains.Add(script.Main);
				}
				if (script.Header != null)
				{
					foreach (var pair in script.Header.pairs)
					{
						if (scripts.TryGetValue((uint)(pair.mainScriptIndex - 1), out var main) && main.Main != null)
						{
							mains.Add(main.Main);
						}
					}
				}
				var plays = new List<object>();
				foreach (var main in mains)
				{
					for (var state = main.scriptState1; state != null; state = state.nextState)
					{
						for (var body = state.scriptStateBody; body != null; body = body.nextScriptStateBody)
						{
							for (var cmd = body.command; cmd != null; cmd = cmd.nextCommand)
							{
								if ((cmd.internalIndex & 0xFFFF) != DoAnim || cmd.arguments == null || cmd.arguments.Count < 6)
								{
									continue;
								}
								uint packed = cmd.arguments[5];
								var clips = Enumerable.Range(0, 4).Select(b => packed >> (8 * b) & 0xFF).Where(b => b != 0xFF)
									.Select(b => $"a{b:D3}").ToList();
								float blend = BitConverter.ToSingle(BitConverter.GetBytes(cmd.arguments[1]), 0);
								plays.Add(new Dictionary<string, object>
								{
									["clips"] = clips,
									["blend"] = blend > 1e-3f && blend < 10f ? blend : 0f,
									["flags"] = cmd.arguments[0],
								});
							}
						}
					}
				}
				if (plays.Count > 0)
				{
					states[slotNames[slot]] = plays;
				}
			}
			return states;
		}

		static (Vector3, Quaternion, Vector3) Evaluate(Animation anim, Animation.JointSettings js, int frame)
		{
			int choice = js.TransformationChoice;
			int st = js.TransformationIndex;
			int at = js.AnimatedTransformIndex;
			var values = anim.AnimatedTransforms[frame];
			float[] v = new float[9];
			for (int c = 0; c < 9; c++)
			{
				bool animated = (choice >> c & 1) == 0;
				bool rotation = c >= 3 && c < 6;
				if (animated)
				{
					v[c] = rotation ? values.GetPureOffset(at++) * 16 / 65536f * (float)(2 * Math.PI) : values.GetOffset(at++);
				}
				else
				{
					var stat = anim.StaticTransforms[st++];
					v[c] = rotation ? stat.GetRot(false) : stat.Value;
				}
			}
			// Row-vector matrices, as the editor builds them: X first, then Y, then Z.
			var m = Matrix4x4.CreateRotationX(v[3]) * Matrix4x4.CreateRotationY(v[4]) * Matrix4x4.CreateRotationZ(v[5]);
			var q = Quaternion.Normalize(Quaternion.CreateFromRotationMatrix(m));
			return (new Vector3(v[0], v[1], v[2]), q, new Vector3(v[6], v[7], v[8]));
		}
	}
}

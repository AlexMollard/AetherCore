using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The camera's up direction, for anything that needs to offset something relative to
/// the view (the tool gun's and physics gun's viewmodels) rather than re-deriving a
/// basis per caller.
///
/// WHY THIS IS NOT A GUESS, and specifically why the +sign here is trustworthy when the
/// general warning "this engine is Y-down" has burned people before: that Y-down rule is
/// a UI-LAYOUT rule only (ui_shapes.slang's ndcY = (sp.y/screenSize.y)*2-1 through an
/// unflipped viewport; see Ui.cs SetRect's remarks). WORLD space is Y-up, and the camera
/// basis proves it in the same breath:
///
/// - Right is the localToWorld matrix's +X column, read out verbatim by
///   aether_camera_get_right (CameraExports.cpp:282: `glm::vec3 right = glm::vec3(pose[0])`).
/// - Forward is the -Z column: InteropCommon.hpp's ForwardOf (line 84) returns
///   `-glm::vec3(m[2])`, and aether_camera_get_forward's no-transform fallback is
///   `{0,0,-1}` (CameraExports.cpp:259) - the standard GLM look-at convention of a
///   camera looking down its own -Z.
/// - A right-handed basis with X = right, Z = backward therefore has Y = up, and
///   up == cross(right, forward): at identity, right = (1,0,0) and forward = (0,0,-1),
///   and (1,0,0) x (0,0,-1) = (0,1,0). World-up corroboration away from the camera:
///   Human.gltf's Hips rest at +y ~104 raw units (PlayerBody's ModelScale comment) and
///   FirstPersonPlayer offsets its camera ABOVE its feet by +EyeHeight.
///
/// There is no aether_camera_get_up export to call instead (checked Camera.cs and
/// CameraExports.cpp) - this derivation is the substitute, built from the engine's own
/// two basis exports rather than any remembered cross-product rule.
/// </summary>
public static class CameraBasis
{
    public static Vector3 Up(Entity camera)
    {
        return Vector3.Cross(Camera.GetRight(camera), Camera.GetForward(camera));
    }
}

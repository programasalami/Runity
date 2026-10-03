using UnityEngine;

namespace WaW.Presentation.World
{
    /// <summary>Top-down orthographic camera that follows the local player and turns freely, at the original's scale: one tile is
    /// 50 screen pixels at zoom 1 (Shift + wheel: 0.5..5), and the player sits in the middle of the area left of the HUD.</summary>
    public sealed class CameraRig : MonoBehaviour
    {
        public Camera Camera;
        public const float PixelsPerTile = 50f;
        public float Zoom = 1f;
        /// <summary>Width of the HUD on the right, in screen pixels (the camera centre moves left by half of it).</summary>
        public float HudWidthPixels;
        public float TurnSpeedDegrees = 172f;  // the original's 0.003 rad/ms

        /// <summary>Camera angle in radians, as the movement input mapping uses it (0 = north up).</summary>
        public float AngleRadians { get; private set; }
        public float AngleDegrees => AngleRadians * Mathf.Rad2Deg;

        public void Init(Camera cam)
        {
            Camera = cam;
            Camera.orthographic = true;
            Camera.orthographicSize = Screen.height / 2f / PixelsPerTile / Zoom;
            Camera.backgroundColor = new Color(0.03f, 0.03f, 0.05f);
            Camera.clearFlags = CameraClearFlags.SolidColor;
        }

        public void Turn(float direction, float deltaTime)
        {
            AngleRadians = Mathf.Repeat(AngleRadians + direction * TurnSpeedDegrees * Mathf.Deg2Rad * deltaTime, Mathf.PI * 2f);
        }

        public void ResetAngle() => AngleRadians = 0f;

        public void ZoomBy(float steps) => Zoom = Mathf.Clamp(Zoom + 0.1f * steps, 0.5f, 5f);

        public void Follow(Vector3 target)
        {
            Camera.orthographicSize = Screen.height / 2f / PixelsPerTile / Zoom;
            // Rotating the camera by -angle shows world direction rotate(up, angle) as "up" on screen.
            var rotation = Quaternion.Euler(0f, 0f, -AngleDegrees);
            // Shift so the player is centred in the play area left of the HUD: move the camera right (on screen) by half the HUD.
            var shiftTiles = HudWidthPixels / 2f / PixelsPerTile / Zoom;
            Camera.transform.position = new Vector3(target.x, target.y, -10f) + rotation * new Vector3(shiftTiles, 0f, 0f);
            Camera.transform.rotation = rotation;
        }
    }
}

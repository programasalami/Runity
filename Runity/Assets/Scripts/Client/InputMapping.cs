using System;

namespace Runity.Client
{
    /// <summary>Turns screen-relative movement input into the world-space direction the server simulates, the way the reference
    /// did (Player.HandleRelativeMovement: the move vector is rotated by the camera angle). World y points down (south).</summary>
    public static class InputMapping
    {
        /// <param name="right">-1..1, right on screen is positive.</param>
        /// <param name="up">-1..1, up on screen is positive.</param>
        /// <param name="cameraAngle">Camera rotation in radians (0 = north up).</param>
        public static (sbyte X, sbyte Y) ToWorldDirection(float right, float up, float cameraAngle)
        {
            // Screen-relative vector in world orientation at angle 0: up on screen = -y (north).
            var rx = right;
            var ry = -up;
            var len = MathF.Sqrt(rx * rx + ry * ry);
            if (len < 0.2f) return (0, 0);  // dead zone
            if (len > 1f)
            {
                rx /= len;
                ry /= len;
            }
            var cos = MathF.Cos(cameraAngle);
            var sin = MathF.Sin(cameraAngle);
            var wx = rx * cos - ry * sin;
            var wy = rx * sin + ry * cos;
            return (Quantize(wx), Quantize(wy));
        }

        public static sbyte Quantize(float v) => (sbyte)Math.Clamp((int)MathF.Round(v * 127f), -127, 127);
    }
}

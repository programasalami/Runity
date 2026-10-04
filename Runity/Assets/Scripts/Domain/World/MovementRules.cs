using System;

namespace Runity.Domain.World
{
    public struct Vec2
    {
        public float X;
        public float Y;

        public Vec2(float x, float y)
        {
            X = x;
            Y = y;
        }

        public override string ToString() => $"({X}, {Y})";
    }

    public struct MoverState
    {
        public Vec2 Position;
        public float SinkLevel;
    }

    /// <summary>Player movement, a line-for-line port of the C++ server's sim::movement (Server/libs/sim/src/movement.cpp) so the
    /// local player's prediction lands exactly where the server will put it. Protocol/vectors/movement.json pins both
    /// implementations together bit for bit. Never change one side without the other.</summary>
    public static class MovementRules
    {
        public const float MinMoveSpeed = 0.004f;
        public const float MaxMoveSpeed = 0.0096f;
        public const int SpeedDivisor = 75;
        public const float MoveThreshold = 0.4f;
        public const float MaxSinkLevel = 18f;
        public const float SinkStepMs = 50f;
        public const float MaxStepMs = 50f;

        // FLOAT RULE: every intermediate result is cast with (float). The cast emits conv.r4, which forces rounding to 32 bits on
        // every runtime; Unity's Mono may otherwise keep intermediates at double precision and drift from the C++ server
        // (found by MovementTests.MatchesTheServerBitForBit, case "mud", in Unity only). Keep the casts when editing.

        public static float MoveSpeed(int speedStat, float tileMultiplier)
        {
            // Integer division on purpose: the reference's formula.
            var steps = speedStat / SpeedDivisor;
            var speed = (float)(MinMoveSpeed + (float)(steps * (float)(MaxMoveSpeed - MinMoveSpeed)));
            return (float)(speed * tileMultiplier);
        }

        public static float TileMultiplier(ClientTile tile, ref float sinkLevel, float dtMs)
        {
            if (tile.Sinking)
            {
                sinkLevel = Math.Min((float)(sinkLevel + (float)(dtMs / SinkStepMs)), MaxSinkLevel);
                return (float)(0.1f + (float)((float)(1.0f - (float)(sinkLevel / MaxSinkLevel)) * (float)(tile.Speed - 0.1f)));
            }
            sinkLevel = 0f;
            return tile.Speed;
        }

        public static Vec2 DirectionFromInput(sbyte dirX, sbyte dirY)
        {
            var d = new Vec2((float)(dirX / 127.0f), (float)(dirY / 127.0f));
            var lenSq = (float)((float)(d.X * d.X) + (float)(d.Y * d.Y));
            if (lenSq > 1.0f)
            {
                var len = (float)MathF.Sqrt(lenSq);
                d.X = (float)(d.X / len);
                d.Y = (float)(d.Y / len);
            }
            return d;
        }

        private static bool IsFullOccupy(ClientTileMap map, float x, float y)
        {
            var tx = (int)x;
            var ty = (int)y;
            if (x < 0f || y < 0f || !map.InBounds(tx, ty)) return true;
            var t = map.At(tx, ty);
            return (t.Flags & (TileFlags.Void | TileFlags.FullOccupy)) != 0;
        }

        private static bool IsWalkable(ClientTile t) => (t.Flags & (TileFlags.NoWalk | TileFlags.OccupySquare)) == 0;

        public static bool IsValidPosition(ClientTileMap map, Vec2 current, float x, float y)
        {
            if (x < 0f || y < 0f) return false;
            var tx = (int)x;
            var ty = (int)y;
            var sameTile = tx == (int)current.X && ty == (int)current.Y;
            if (!sameTile && (!map.InBounds(tx, ty) || !IsWalkable(map.At(tx, ty)))) return false;

            var xFrac = (float)(x - tx);
            var yFrac = (float)(y - ty);
            if (xFrac < 0.5f)
            {
                if (IsFullOccupy(map, (float)(x - 1), y)) return false;
                if (yFrac < 0.5f)
                {
                    if (IsFullOccupy(map, x, (float)(y - 1)) || IsFullOccupy(map, (float)(x - 1), (float)(y - 1))) return false;
                }
                else if (yFrac > 0.5f)
                {
                    if (IsFullOccupy(map, x, (float)(y + 1)) || IsFullOccupy(map, (float)(x - 1), (float)(y + 1))) return false;
                }
            }
            else if (xFrac > 0.5f)
            {
                if (IsFullOccupy(map, (float)(x + 1), y)) return false;
                if (yFrac < 0.5f)
                {
                    if (IsFullOccupy(map, x, (float)(y - 1)) || IsFullOccupy(map, (float)(x + 1), (float)(y - 1))) return false;
                }
                else if (yFrac > 0.5f)
                {
                    if (IsFullOccupy(map, x, (float)(y + 1)) || IsFullOccupy(map, (float)(x + 1), (float)(y + 1))) return false;
                }
            }
            else if (yFrac < 0.5f)
            {
                if (IsFullOccupy(map, x, (float)(y - 1))) return false;
            }
            else if (yFrac > 0.5f)
            {
                if (IsFullOccupy(map, x, (float)(y + 1))) return false;
            }
            return true;
        }

        private static Vec2 ModifyStep(ClientTileMap map, Vec2 pos, float x, float y)
        {
            var xCross = ((float)(pos.X % 0.5f) == 0f && x != pos.X) || (int)(float)(pos.X / 0.5f) != (int)(float)(x / 0.5f);
            var yCross = ((float)(pos.Y % 0.5f) == 0f && y != pos.Y) || (int)(float)(pos.Y / 0.5f) != (int)(float)(y / 0.5f);

            if ((!xCross && !yCross) || IsValidPosition(map, pos, x, y)) return new Vec2(x, y);

            var nextXBorder = 0f;
            var nextYBorder = 0f;
            if (xCross)
            {
                nextXBorder = x > pos.X ? (float)((int)(float)(x * 2) / 2.0f) : (float)((int)(float)(pos.X * 2) / 2.0f);
                if ((int)nextXBorder > (int)pos.X) nextXBorder = (float)(nextXBorder - 0.01f);
            }
            if (yCross)
            {
                nextYBorder = y > pos.Y ? (float)((int)(float)(y * 2) / 2.0f) : (float)((int)(float)(pos.Y * 2) / 2.0f);
                if ((int)nextYBorder > (int)pos.Y) nextYBorder = (float)(nextYBorder - 0.01f);
            }
            if (!xCross) return new Vec2(x, nextYBorder);
            if (!yCross) return new Vec2(nextXBorder, y);

            var xBorderDist = x > pos.X ? (float)(x - nextXBorder) : (float)(nextXBorder - x);
            var yBorderDist = y > pos.Y ? (float)(y - nextYBorder) : (float)(nextYBorder - y);
            if (xBorderDist > yBorderDist)
            {
                if (IsValidPosition(map, pos, x, nextYBorder)) return new Vec2(x, nextYBorder);
                if (IsValidPosition(map, pos, nextXBorder, y)) return new Vec2(nextXBorder, y);
            }
            else
            {
                if (IsValidPosition(map, pos, nextXBorder, y)) return new Vec2(nextXBorder, y);
                if (IsValidPosition(map, pos, x, nextYBorder)) return new Vec2(x, nextYBorder);
            }
            return new Vec2(nextXBorder, nextYBorder);
        }

        public static Vec2 ResolveMove(ClientTileMap map, Vec2 current, float x, float y)
        {
            var dx = (float)(x - current.X);
            var dy = (float)(y - current.Y);
            if (dx < MoveThreshold && dx > -MoveThreshold && dy < MoveThreshold && dy > -MoveThreshold)
                return ModifyStep(map, current, x, y);

            var result = current;
            var stepSize = (float)(MoveThreshold / Math.Max(Math.Abs(dx), Math.Abs(dy)));
            var doneFraction = 0f;
            var done = false;
            while (!done)
            {
                if ((float)(doneFraction + stepSize) >= 1f)
                {
                    stepSize = (float)(1f - doneFraction);
                    done = true;
                }
                result = ModifyStep(map, result, (float)(result.X + (float)(dx * stepSize)), (float)(result.Y + (float)(dy * stepSize)));
                doneFraction = (float)(doneFraction + stepSize);
            }
            return result;
        }

        public static void Step(ClientTileMap map, ref MoverState state, Vec2 direction, float dtMs, int speedStat)
        {
            dtMs = Math.Clamp(dtMs, 0f, MaxStepMs);
            var tile = map.At((int)state.Position.X, (int)state.Position.Y);
            var multiplier = TileMultiplier(tile, ref state.SinkLevel, dtMs);
            if (direction.X == 0f && direction.Y == 0f) return;
            var speed = MoveSpeed(speedStat, multiplier);
            state.Position = ResolveMove(map, state.Position, (float)(state.Position.X + (float)((float)(direction.X * speed) * dtMs)),
                (float)(state.Position.Y + (float)((float)(direction.Y * speed) * dtMs)));
        }
    }
}

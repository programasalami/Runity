using System;
using System.Collections.Generic;
using WaW.Protocol;

namespace WaW.Domain.World
{
    /// <summary>Client-side prediction for the local player. Input is sampled into fixed 16 ms steps, each applied locally at once
    /// (no input lag) and sent to the server with a sequence number. When a Snapshot acknowledges a step, the prediction is rebuilt
    /// from the server's position by replaying the steps the server has not applied yet. With identical rules on both sides
    /// (MovementRules == sim::movement) the replay lands exactly on the prediction unless the server disagreed.</summary>
    public sealed class LocalPlayerPredictor
    {
        public const float StepMs = 16f;
        private const int MaxPending = 240;  // the server's WorldRules::max_pending_steps
        private const float MaxFrameMs = 250f;

        private struct Pending
        {
            public MoveStep Step;
            public MoverState After;  // predicted state after this step (keeps the sink level for replays)
        }

        private readonly List<Pending> _pending = new List<Pending>();
        private readonly List<MoveStep> _outbox = new List<MoveStep>();
        private MoverState _predicted;
        private MoverState _previous;
        private float _accumulator;
        private uint _nextSeq = 1;
        private int _speed;

        public Vec2 Position => _predicted.Position;
        public int PendingCount => _pending.Count;
        /// <summary>Distance the last reconciliation moved the prediction (0 when the server agreed).</summary>
        public float LastCorrection { get; private set; }
        public int Corrections { get; private set; }

        /// <summary>The Speed stat changed (level-up, gear): later steps use the new value, exactly like the server.</summary>
        public void SetSpeed(int speed) => _speed = speed;

        public void Reset(Vec2 position, int speed)
        {
            _predicted = new MoverState { Position = position };
            _previous = _predicted;
            _pending.Clear();
            _outbox.Clear();
            _accumulator = 0f;
            _speed = speed;
            LastCorrection = 0f;
        }

        /// <summary>Advances by one frame with the current input direction (already quantized, world space).</summary>
        public void Advance(ClientTileMap map, float frameMs, sbyte dirX, sbyte dirY)
        {
            _accumulator += Math.Min(frameMs, MaxFrameMs);
            while (_accumulator >= StepMs)
            {
                _accumulator -= StepMs;
                if (_pending.Count >= MaxPending) continue;  // the server is not answering; do not flood it
                var step = new MoveStep { Seq = _nextSeq++, DtMs = (byte)StepMs, DirX = dirX, DirY = dirY };
                _previous = _predicted;
                MovementRules.Step(map, ref _predicted, MovementRules.DirectionFromInput(dirX, dirY), StepMs, _speed);
                _pending.Add(new Pending { Step = step, After = _predicted });
                _outbox.Add(step);
            }
        }

        /// <summary>The steps produced since the last call (send them in one MoveInput).</summary>
        public List<MoveStep> TakeOutbox()
        {
            var steps = new List<MoveStep>(_outbox);
            _outbox.Clear();
            return steps;
        }

        /// <summary>Smooth render position between the last two steps.</summary>
        public Vec2 RenderPosition()
        {
            var t = _accumulator / StepMs;
            return new Vec2(_previous.Position.X + (_predicted.Position.X - _previous.Position.X) * t,
                _previous.Position.Y + (_predicted.Position.Y - _previous.Position.Y) * t);
        }

        /// <summary>Applies the server's verdict: its position after step `ackSeq`.</summary>
        public void Reconcile(ClientTileMap map, uint ackSeq, Vec2 serverPosition)
        {
            var acked = -1;
            for (var i = 0; i < _pending.Count; i++)
            {
                if (_pending[i].Step.Seq <= ackSeq) acked = i;
                else break;
            }
            var basis = new MoverState { Position = serverPosition, SinkLevel = acked >= 0 ? _pending[acked].After.SinkLevel : _predicted.SinkLevel };
            if (acked >= 0) _pending.RemoveRange(0, acked + 1);

            var before = _predicted.Position;
            var state = basis;
            for (var i = 0; i < _pending.Count; i++)
            {
                var p = _pending[i];
                MovementRules.Step(map, ref state, MovementRules.DirectionFromInput(p.Step.DirX, p.Step.DirY), p.Step.DtMs, _speed);
                p.After = state;
                _pending[i] = p;
            }
            var dx = state.Position.X - before.X;
            var dy = state.Position.Y - before.Y;
            LastCorrection = MathF.Sqrt(dx * dx + dy * dy);
            if (LastCorrection > 0f)
            {
                Corrections++;
                // Snap the previous frame by the same offset so the correction does not render as a jump backwards in time.
                _previous.Position = new Vec2(_previous.Position.X + dx, _previous.Position.Y + dy);
            }
            _predicted = state;
        }
    }
}

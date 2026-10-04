using System;
using System.Collections.Generic;

namespace Runity.Domain.World
{
    /// <summary>Estimates the server's tick on the local clock from Snapshot arrivals (smoothed, never runs backwards).</summary>
    public sealed class ServerClock
    {
        public const float TickMs = 50f;
        private double _estimate;
        private bool _started;

        public double Tick => _estimate;

        public void OnSnapshot(uint serverTick)
        {
            if (!_started || Math.Abs(serverTick - _estimate) > 20)
            {
                _estimate = serverTick;  // first sample or a big jump (world change, long stall): adopt it
                _started = true;
                return;
            }
            // Pull gently towards the arrival; late packets must not drag the clock back much.
            _estimate += (serverTick - _estimate) * 0.1;
        }

        public void Advance(float frameMs)
        {
            if (_started) _estimate += frameMs / TickMs;
        }
    }

    /// <summary>Position history of one remote entity, rendered a little in the past so there are always two samples to blend
    /// (entities move on the server at 20 ticks per second; the screen runs much faster).</summary>
    public sealed class PositionBuffer
    {
        private struct Entry
        {
            public double Tick;
            public Vec2 Position;
        }

        private readonly List<Entry> _samples = new List<Entry>(8);
        private const int MaxSamples = 16;

        public void Add(uint tick, Vec2 position)
        {
            if (_samples.Count > 0)
            {
                var last = _samples[_samples.Count - 1];
                if (tick <= last.Tick) return;  // out of order or duplicate
                // The entity stood still for a while (no updates are sent then): hold the old position until one tick before the
                // new one, so the move is not smeared over the whole pause.
                if (tick - last.Tick > 1) _samples.Add(new Entry { Tick = tick - 1, Position = last.Position });
            }
            _samples.Add(new Entry { Tick = tick, Position = position });
            if (_samples.Count > MaxSamples) _samples.RemoveRange(0, _samples.Count - MaxSamples);
        }

        public bool HasSamples => _samples.Count > 0;

        public Vec2 Latest => _samples[_samples.Count - 1].Position;

        public Vec2 Sample(double renderTick)
        {
            if (_samples.Count == 0) return default;
            if (renderTick <= _samples[0].Tick) return _samples[0].Position;
            for (var i = 1; i < _samples.Count; i++)
            {
                var b = _samples[i];
                if (renderTick <= b.Tick)
                {
                    var a = _samples[i - 1];
                    var t = (float)((renderTick - a.Tick) / (b.Tick - a.Tick));
                    return new Vec2(a.Position.X + (b.Position.X - a.Position.X) * t, a.Position.Y + (b.Position.Y - a.Position.Y) * t);
                }
            }
            return Latest;  // never extrapolate past the newest sample
        }
    }
}

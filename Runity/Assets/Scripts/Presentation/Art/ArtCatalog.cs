using System;
using System.Collections.Generic;
using UnityEngine;

namespace Runity.Presentation.Art
{
    /// <summary>Maps the definitions' art keys ("sheet:index") to sprites: an explicit entry here wins (one-off overrides), otherwise
    /// the original sprite sheets in Content/Art (<see cref="SheetAtlas"/>); with neither, callers draw a placeholder.</summary>
    [CreateAssetMenu(menuName = "Runity/Art Catalog", fileName = "ArtCatalog")]
    public sealed class ArtCatalog : ScriptableObject
    {
        [Serializable]
        public struct Entry
        {
            public string Key;
            public Sprite Sprite;
        }

        public List<Entry> Entries = new List<Entry>();
        private Dictionary<string, Sprite> _lookup;

        public Sprite Find(string key)
        {
            if (string.IsNullOrEmpty(key)) return null;
            return Lookup().TryGetValue(key, out var s) ? s : SheetAtlas.Shared?.Find(key);
        }

        /// <summary>Like Find, for ground and wall pictures: a sheet picture comes on its own texture (SheetAtlas.Tile). An entry here
        /// must likewise be the only picture on its texture (walls map the whole texture onto their faces).</summary>
        public Sprite FindTile(string key)
        {
            if (string.IsNullOrEmpty(key)) return null;
            return Lookup().TryGetValue(key, out var s) ? s : SheetAtlas.Shared?.Tile(key);
        }

        private Dictionary<string, Sprite> Lookup()
        {
            if (_lookup != null) return _lookup;
            _lookup = new Dictionary<string, Sprite>();
            foreach (var e in Entries)
            {
                if (!string.IsNullOrEmpty(e.Key) && e.Sprite != null) _lookup[e.Key] = e.Sprite;
            }
            return _lookup;
        }
    }

    /// <summary>Runtime-made stand-ins: a white square and a white disc tinted per type, so a missing picture is obvious but every
    /// ground / object stays distinguishable.</summary>
    public static class Placeholders
    {
        private static Sprite _square;
        private static Sprite _disc;

        public static Sprite Square => _square != null ? _square : (_square = MakeSquare());
        public static Sprite Disc => _disc != null ? _disc : (_disc = MakeDisc());

        private static Sprite MakeSquare()
        {
            var tex = new Texture2D(4, 4) { filterMode = FilterMode.Point, name = "PlaceholderSquare" };
            var pixels = new Color32[16];
            for (var i = 0; i < pixels.Length; i++) pixels[i] = new Color32(255, 255, 255, 255);
            tex.SetPixels32(pixels);
            tex.Apply();
            return Sprite.Create(tex, new Rect(0, 0, 4, 4), new Vector2(0.5f, 0.5f), 4f);
        }

        private static Sprite MakeDisc()
        {
            const int size = 32;
            var tex = new Texture2D(size, size) { filterMode = FilterMode.Bilinear, name = "PlaceholderDisc" };
            var pixels = new Color32[size * size];
            var r = size / 2f - 1f;
            for (var y = 0; y < size; y++)
            {
                for (var x = 0; x < size; x++)
                {
                    var dx = x + 0.5f - size / 2f;
                    var dy = y + 0.5f - size / 2f;
                    var d = Mathf.Sqrt(dx * dx + dy * dy);
                    var a = Mathf.Clamp01(r - d + 0.5f);
                    pixels[y * size + x] = new Color32(255, 255, 255, (byte)(a * 255));
                }
            }
            tex.SetPixels32(pixels);
            tex.Apply();
            return Sprite.Create(tex, new Rect(0, 0, size, size), new Vector2(0.5f, 0.5f), size);
        }

        /// <summary>A stable colour per id (same id, same colour, every run).</summary>
        public static Color ColorFor(string id, float saturation = 0.45f, float value = 0.75f)
        {
            unchecked
            {
                var h = 2166136261u;
                foreach (var c in id ?? "") h = (h ^ c) * 16777619u;
                return Color.HSVToRGB((h % 1000) / 1000f, saturation, value);
            }
        }
    }
}

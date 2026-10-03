using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Xml.Linq;
using UnityEngine;

namespace WaW.Presentation.Art
{
    public enum Facing
    {
        Right = 0,
        Down = 1,
        Up = 2,
    }

    /// <summary>The original client's sprite sheets (Content/Art: Game.atlas + Sheets/*.png), cut at runtime with AlloyClient's own
    /// rules (Alloy.ContentBuilder AtlasBuilder.ParseSheet / ParseAnimated): a static index is row-major in cells of the entry's size;
    /// an animated sheet has 7 columns per row (stand, walk 1, walk 2, unused, attack 1, attack 2 double width) and, for group "Full",
    /// 3 rows per index (right, down, up). PNGs load on first use, so only the sheets a map needs are ever read.</summary>
    public sealed class SheetAtlas
    {
        public const int StandFrame = 0;
        public const int AttackFrame = 4;

        private sealed class Entry
        {
            public string File;
            public int CellW;
            public int CellH;
            public bool Animated;
            public int Group = 1;
        }

        private readonly string _sheetDir;
        private readonly Dictionary<string, Entry> _entries = new Dictionary<string, Entry>(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, Texture2D> _textures = new Dictionary<string, Texture2D>(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, Sprite> _sprites = new Dictionary<string, Sprite>();
        private readonly Dictionary<Texture2D, Color32[]> _pixels = new Dictionary<Texture2D, Color32[]>();

        public static SheetAtlas Shared { get; set; }

        public SheetAtlas(string artDirectory)
        {
            var doc = XDocument.Load(Path.Combine(artDirectory, "Game.atlas"));
            _sheetDir = Path.Combine(artDirectory, (string)doc.Root.Attribute("source") ?? "Sheets");
            foreach (var node in doc.Root.Elements())
            {
                var name = (string)node.Attribute("name");
                if (string.IsNullOrEmpty(name)) continue;
                _entries[name] = new Entry
                {
                    File = node.Value.Trim(),
                    CellW = (int?)node.Attribute("w") ?? 0,
                    CellH = (int?)node.Attribute("h") ?? 0,
                    Animated = node.Name.LocalName == "Animated",
                    Group = (string)node.Attribute("group") == "Full" ? 3 : 1,
                };
            }
        }

        public int EntryCount => _entries.Count;

        /// <summary>The picture for an art key "sheet:index" as the definitions write it (index hex "0x2d" or decimal). For an animated
        /// sheet this is the standing frame facing right, as AlloyClient uses for icons. Null when the sheet or cell does not exist.</summary>
        public Sprite Find(string key)
        {
            if (string.IsNullOrEmpty(key)) return null;
            var colon = key.LastIndexOf(':');
            if (colon <= 0 || !TryParseIndex(key.Substring(colon + 1), out var index)) return null;
            var sheet = key.Substring(0, colon);
            if (!_entries.TryGetValue(sheet, out var e)) return null;
            return e.Animated ? Frame(sheet, index, Facing.Right, StandFrame) : Static(sheet, index);
        }

        private readonly Dictionary<string, Color32> _dominant = new Dictionary<string, Color32>();

        /// <summary>The most frequent opaque colour of a picture (the original's minimap colour per tile / object). Clear when the
        /// picture does not exist.</summary>
        public Color32 Dominant(string key)
        {
            if (string.IsNullOrEmpty(key)) return default;
            if (_dominant.TryGetValue(key, out var c)) return c;
            var sprite = Find(key);
            c = default;
            if (sprite != null && _pixels.TryGetValue(sprite.texture, out var pixels))
            {
                var r = sprite.rect;
                var counts = new Dictionary<int, int>();
                var best = 0;
                for (var y = (int)r.y; y < (int)r.yMax; y++)
                {
                    for (var x = (int)r.x; x < (int)r.xMax; x++)
                    {
                        var p = pixels[y * sprite.texture.width + x];
                        if (p.a == 0) continue;
                        var rgb = p.r << 16 | p.g << 8 | p.b;
                        counts.TryGetValue(rgb, out var n);
                        counts[rgb] = ++n;
                        if (n > best)
                        {
                            best = n;
                            c = new Color32(p.r, p.g, p.b, 255);
                        }
                    }
                }
            }
            _dominant[key] = c;
            return c;
        }

        public bool IsAnimated(string key)
        {
            var colon = key?.LastIndexOf(':') ?? -1;
            return colon > 0 && _entries.TryGetValue(key.Substring(0, colon), out var e) && e.Animated;
        }

        public Sprite Static(string sheet, int index)
        {
            if (!_entries.TryGetValue(sheet, out var e)) return null;
            var cacheKey = sheet + ":" + index;
            if (_sprites.TryGetValue(cacheKey, out var cached)) return cached;
            var tex = Texture(e.File);
            Sprite sprite = null;
            if (tex != null)
            {
                var w = e.CellW > 0 ? e.CellW : tex.width;
                var h = e.CellH > 0 ? e.CellH : tex.height;
                var cols = Math.Max(1, tex.width / w);
                var x = index % cols * w;
                var top = index / cols * h;
                if (top + h <= tex.height) sprite = Cut(tex, x, top, w, h, new Vector2(0.5f, 0.5f), w, cacheKey);
            }
            _sprites[cacheKey] = sprite;
            return sprite;
        }

        /// <summary>One animation frame of a character (0 stand, 1-2 walk, 4 attack 1, 5 attack 2 which is two cells wide). Facing
        /// left is Right drawn mirrored. Pivot is the bottom centre of the first cell so attack frames extend forward.</summary>
        public Sprite Frame(string sheet, int index, Facing facing, int frame)
        {
            if (!_entries.TryGetValue(sheet, out var e) || !e.Animated) return null;
            var cacheKey = sheet + ":" + index + ":" + (int)facing + ":" + frame;
            if (_sprites.TryGetValue(cacheKey, out var cached)) return cached;
            var tex = Texture(e.File);
            Sprite sprite = null;
            if (tex != null && e.CellW > 0 && e.CellH > 0)
            {
                var framesPerRow = tex.width / e.CellW - 1;
                var row = index * e.Group + (e.Group == 3 ? (int)facing : 0);
                var top = row * e.CellH;
                if (frame >= 0 && frame < framesPerRow && top + e.CellH <= tex.height)
                {
                    var width = frame == framesPerRow - 1 ? 2 * e.CellW : e.CellW;
                    if (frame == 2 && IsEmpty(tex, 2 * e.CellW, top, e.CellW, e.CellH)) return Frame(sheet, index, facing, 0);
                    var pivot = new Vector2(0.5f * e.CellW / width, 0f);
                    sprite = Cut(tex, frame * e.CellW, top, width, e.CellH, pivot, e.CellW, cacheKey);
                }
            }
            _sprites[cacheKey] = sprite;
            return sprite;
        }

        private Texture2D Texture(string file)
        {
            if (_textures.TryGetValue(file, out var tex)) return tex;
            var path = Path.Combine(_sheetDir, file);
            if (File.Exists(path))
            {
                tex = new Texture2D(2, 2, TextureFormat.RGBA32, false) { filterMode = FilterMode.Point, wrapMode = TextureWrapMode.Clamp, name = file };
                if (!tex.LoadImage(File.ReadAllBytes(path), false)) tex = null;
            }
            if (tex == null) Debug.LogWarning($"[WaW] art: sheet '{file}' not found in {_sheetDir}");
            _textures[file] = tex;
            return tex;
        }

        /// <summary>Cells are addressed top-down like the PNG; Unity textures are bottom-up. One cell width is one world unit (one
        /// tile), so a double-width attack frame is two units wide at the same scale. Fully transparent cells give no sprite.</summary>
        private Sprite Cut(Texture2D tex, int x, int top, int w, int h, Vector2 pivot, int pixelsPerUnit, string name)
        {
            if (IsEmpty(tex, x, top, w, h)) return null;
            var sprite = Sprite.Create(tex, new Rect(x, tex.height - top - h, w, h), pivot, pixelsPerUnit, 0, SpriteMeshType.FullRect);
            sprite.name = name;
            return sprite;
        }

        private bool IsEmpty(Texture2D tex, int x, int top, int w, int h)
        {
            if (!_pixels.TryGetValue(tex, out var pixels)) _pixels[tex] = pixels = tex.GetPixels32();
            for (var yy = 0; yy < h; yy++)
            {
                var row = (tex.height - top - h + yy) * tex.width;
                for (var xx = 0; xx < w; xx++)
                {
                    if (pixels[row + x + xx].a != 0) return false;
                }
            }
            return true;
        }

        private static bool TryParseIndex(string text, out int index)
        {
            text = text.Trim();
            if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
                return int.TryParse(text.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out index);
            return int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out index);
        }
    }
}

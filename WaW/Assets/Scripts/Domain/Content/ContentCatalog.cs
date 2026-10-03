using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Xml.Linq;

namespace WaW.Domain.Content
{
    public sealed class GroundDef
    {
        public ushort Type;
        public string Id;
        public bool NoWalk;
        public float Speed = 1f;
        public bool Sinking;
        /// <summary>&lt;RandomTexture&gt; alternatives (one per tile, stable by position).</summary>
        public List<string> RandomArtKeys;
        /// <summary>Art key for presentation (the reference's &lt;Texture&gt; File:Index); never used by gameplay.</summary>
        public string ArtKey;
    }

    public sealed class ObjectDef
    {
        public ushort Type;
        public string Id;
        public string Class = "";
        public bool Static;
        public bool OccupySquare;
        public bool FullOccupy;
        public bool EnemyOccupySquare;
        public bool BlocksSight;
        public bool Enemy;
        public bool Player;
        public bool Item;
        public int Size = 100;
        public string DisplayName;
        public string ArtKey;
        /// <summary>True when the picture is an &lt;AnimatedTexture&gt; (characters: facing + walk / attack frames).</summary>
        public bool Animated;
        /// <summary>&lt;Top&gt;&lt;Texture&gt; of a wall: drawn on its top face.</summary>
        public string TopArtKey;
        /// <summary>&lt;RandomTexture&gt;: one is picked per placement.</summary>
        public List<string> RandomArtKeys;
        /// <summary>Projectiles: &lt;AngleCorrection&gt; in eighths of a turn added to the flight direction, and &lt;Rotation&gt;
        /// (non-zero: the sprite spins at elapsedMs / Rotation radians instead).</summary>
        public int AngleCorrection;
        public float Rotation;

        /// <summary>Same rule as the server (TileMap::is_tile_object): static decoration and walls travel with the tile data.</summary>
        public bool IsTileObject => Static || Class == "Wall";
    }

    public enum PathKind : byte
    {
        Line = 0,
        Amplitude = 1,
        Wavy = 2,
        Boomerang = 3,
    }

    /// <summary>A weapon's projectile (the server's content::ProjectileDesc; speed in tiles per second = XML Speed / 10).</summary>
    public sealed class ProjectileDef
    {
        public string ObjectId;
        public ushort ObjectType;
        public PathKind Path;
        public float Speed;
        public int LifetimeMs;
        public float Amplitude;
        public float Frequency = 1f;
        public int Size = 100;
    }

    public sealed class ItemDef
    {
        public ushort Type;
        public string Id;
        public string DisplayName;
        public int SlotType;
        public int Tier = -1;
        public float RateOfFire = 1f;
        public int NumProjectiles = 1;
        public float ArcGapDegrees = 11.25f;
        public ProjectileDef Projectile;
        public bool Consumable;
        public string ArtKey;
        public string Description;
        public bool Soulbound => BagType > 0;
        public int BagType;
        /// <summary>What wearing it adds, by stat name ("Defense" -> 2). From &lt;ActivateOnEquip stat amount&gt; (reference stat ids).</summary>
        public readonly List<KeyValuePair<string, int>> Bonuses = new List<KeyValuePair<string, int>>();
        public int HealAmount;
        public int MagicAmount;
    }

    public sealed class ClassDef
    {
        public ushort Type;
        public string Id;
        public string Description;
        public int StartSpeed;
        /// <summary>The class's &lt;AnimatedTexture&gt; (sheet "players" and its index): portraits and the walking body.</summary>
        public string AnimatedSheet;
        public int AnimatedIndex;
        /// <summary>&lt;Equipment&gt;: starting items per slot (-1 = empty); the first 4 are weapon, ability, armor, ring.</summary>
        public readonly List<int> Equipment = new List<int>();
        public readonly List<int> SlotTypes = new List<int>();
        /// <summary>The stats' max="" attributes (MaxHitPoints, MaxMagicPoints, Attack, Defense, Speed, Dexterity, HpRegen, MpRegen).</summary>
        public readonly Dictionary<string, int> MaxStats = new Dictionary<string, int>();
    }

    /// <summary>The gameplay definitions the client needs for prediction and presentation, read from the SAME files the C++ server
    /// loads (Content/Definitions). The server stays authoritative; the client never decides an outcome from these values.</summary>
    public sealed class ContentCatalog
    {
        private readonly Dictionary<ushort, GroundDef> _grounds = new Dictionary<ushort, GroundDef>();
        private readonly Dictionary<ushort, ObjectDef> _objects = new Dictionary<ushort, ObjectDef>();
        private readonly Dictionary<ushort, ClassDef> _classes = new Dictionary<ushort, ClassDef>();
        private readonly Dictionary<ushort, ItemDef> _items = new Dictionary<ushort, ItemDef>();
        private readonly HashSet<string> _groundIds = new HashSet<string>();
        private readonly Dictionary<string, ObjectDef> _objectsById = new Dictionary<string, ObjectDef>();

        /// <summary>Definitions skipped because an earlier file already defined that type or id (first wins).</summary>
        public int SkippedDuplicates { get; private set; }

        public ObjectDef ObjectById(string id) => id != null && _objectsById.TryGetValue(id, out var o) ? o : null;

        public IReadOnlyCollection<GroundDef> Grounds => _grounds.Values;
        public IReadOnlyCollection<ObjectDef> Objects => _objects.Values;
        public IReadOnlyCollection<ClassDef> Classes => _classes.Values;

        public GroundDef Ground(ushort type) => _grounds.TryGetValue(type, out var g) ? g : null;
        public ObjectDef Object(ushort type) => _objects.TryGetValue(type, out var o) ? o : null;
        public ClassDef Class(ushort type) => _classes.TryGetValue(type, out var c) ? c : null;
        public ItemDef Item(int type) => type >= 0 && type <= ushort.MaxValue && _items.TryGetValue((ushort)type, out var i) ? i : null;
        public IReadOnlyCollection<ItemDef> Items => _items.Values;

        /// <summary>Loads every *.xml in a directory (sorted, like the server).</summary>
        public static ContentCatalog LoadDirectory(string definitionsDirectory)
        {
            var catalog = new ContentCatalog();
            foreach (var file in Directory.GetFiles(definitionsDirectory, "*.xml").OrderBy(f => f, StringComparer.Ordinal))
                catalog.AddXml(File.ReadAllText(file), Path.GetFileName(file));
            catalog.Link();
            return catalog;
        }

        /// <summary>Resolves names across files (a weapon's projectile look lives in Projectiles.xml).</summary>
        public void Link()
        {
            foreach (var item in _items.Values)
            {
                if (item.Projectile == null) continue;
                var look = ObjectById(item.Projectile.ObjectId);
                if (look == null || look.Class != "Projectile")
                {
                    item.Projectile = null;  // the server drops it too: the weapon does not fire
                    continue;
                }
                item.Projectile.ObjectType = look.Type;
            }
        }

        public void AddXml(string xml, string source)
        {
            XElement root;
            try
            {
                root = XDocument.Parse(xml).Root;
            }
            catch (System.Xml.XmlException e)
            {
                throw new FormatException(source + ": " + e.Message);
            }
            if (root == null) return;
            foreach (var node in root.Elements())
            {
                switch (node.Name.LocalName)
                {
                    case "Ground": AddGround(node, source); break;
                    case "Object": AddObject(node, source); break;
                }
            }
        }

        private static ushort ParseType(XElement node, string source)
        {
            var text = (string)node.Attribute("type") ?? "";
            if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase) &&
                ushort.TryParse(text.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var hex)) return hex;
            if (ushort.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var dec)) return dec;
            throw new FormatException($"{source}: bad type '{text}' on '{(string)node.Attribute("id")}'");
        }

        private static List<string> RandomArtKeys(XElement node)
        {
            var random = node.Element("RandomTexture");
            if (random == null) return null;
            var keys = new List<string>();
            foreach (var t in random.Elements())
            {
                var file = (string)t.Element("File");
                if (file != null) keys.Add(file.Trim() + ":" + (((string)t.Element("Index"))?.Trim() ?? "0"));
            }
            return keys.Count > 0 ? keys : null;
        }

        private static string ArtKey(XElement node)
        {
            if (node == null) return null;
            var texture = node.Element("Texture") ?? node.Element("AnimatedTexture");
            if (texture == null) return null;
            var file = ((string)texture.Element("File"))?.Trim();
            var index = ((string)texture.Element("Index"))?.Trim();
            return file == null ? null : file + ":" + (index ?? "0");
        }

        private static float ParseFloat(XElement e, float fallback) =>
            e != null && float.TryParse(e.Value.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : fallback;

        private void AddGround(XElement node, string source)
        {
            var g = new GroundDef
            {
                Type = ParseType(node, source),
                Id = (string)node.Attribute("id"),
                NoWalk = node.Element("NoWalk") != null,
                Sinking = node.Element("Sinking") != null,
                Speed = ParseFloat(node.Element("Speed"), 1f),
                ArtKey = ArtKey(node),
            };
            if (_grounds.ContainsKey(g.Type) || !_groundIds.Add(g.Id ?? ""))
            {
                SkippedDuplicates++;
                return;
            }
            g.RandomArtKeys = RandomArtKeys(node);
            _grounds[g.Type] = g;
        }

        private void AddObject(XElement node, string source)
        {
            var cls = ((string)node.Element("Class") ?? "").Trim();
            var o = new ObjectDef
            {
                Type = ParseType(node, source),
                Id = (string)node.Attribute("id"),
                Class = cls,
                Static = node.Element("Static") != null,
                OccupySquare = node.Element("OccupySquare") != null,
                FullOccupy = node.Element("FullOccupy") != null,
                EnemyOccupySquare = node.Element("EnemyOccupySquare") != null,
                BlocksSight = node.Element("BlocksSight") != null || cls == "Wall" || cls == "CaveWall" || cls == "ConnectedWall",
                Enemy = node.Element("Enemy") != null,
                Player = node.Element("Player") != null,
                Item = node.Element("Item") != null,
                Size = (int)ParseFloat(node.Element("Size"), 100f),
                DisplayName = ((string)node.Element("DisplayId"))?.Trim(),
                ArtKey = ArtKey(node),
            };
            if (o.Type == 0 || _objects.ContainsKey(o.Type) || o.Id == null || _objectsById.ContainsKey(o.Id))
            {
                SkippedDuplicates++;
                return;
            }
            o.Animated = node.Element("AnimatedTexture") != null;
            o.AngleCorrection = ParseInt(node.Element("AngleCorrection"), 0);
            o.Rotation = ParseFloat(node.Element("Rotation"), 0f);
            o.TopArtKey = ArtKey(node.Element("Top"));
            o.RandomArtKeys = RandomArtKeys(node);
            _objects[o.Type] = o;
            _objectsById[o.Id] = o;
            if (o.Item) _items[o.Type] = ParseItem(node, o);
            if (o.Player)
            {
                _classes[o.Type] = new ClassDef
                {
                    Type = o.Type,
                    Id = o.Id,
                    Description = ((string)node.Element("Description"))?.Trim(),
                    StartSpeed = (int)ParseFloat(node.Element("Speed"), 0f),
                };
                var classDef = _classes[o.Type];
                var anim = node.Element("AnimatedTexture");
                if (anim != null)
                {
                    classDef.AnimatedSheet = ((string)anim.Element("File"))?.Trim();
                    classDef.AnimatedIndex = ParseInt(anim.Element("Index"), 0);
                }
                classDef.Equipment.AddRange(ParseIntList((string)node.Element("Equipment")));
                classDef.SlotTypes.AddRange(ParseIntList((string)node.Element("SlotTypes")));
                foreach (var stat in node.Elements())
                {
                    var max = (string)stat.Attribute("max");
                    if (max != null && int.TryParse(max, NumberStyles.Integer, CultureInfo.InvariantCulture, out var m))
                        classDef.MaxStats[stat.Name.LocalName] = m;
                }
            }
        }

        private static IEnumerable<int> ParseIntList(string csv)
        {
            if (string.IsNullOrWhiteSpace(csv)) yield break;
            foreach (var part in csv.Split(','))
            {
                var t = part.Trim();
                if (t.StartsWith("0x", StringComparison.OrdinalIgnoreCase) &&
                    int.TryParse(t.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var hex)) yield return hex;
                else if (int.TryParse(t, NumberStyles.Integer, CultureInfo.InvariantCulture, out var v)) yield return v;
            }
        }

        private static int ParseInt(XElement e, int fallback)
        {
            if (e == null) return fallback;
            var text = e.Value.Trim();
            if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase) &&
                int.TryParse(text.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var hex)) return hex;
            return int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var v) ? v : fallback;
        }

        private static ItemDef ParseItem(XElement node, ObjectDef o)
        {
            var item = new ItemDef
            {
                Type = o.Type,
                Id = o.Id,
                DisplayName = o.DisplayName ?? o.Id,
                SlotType = ParseInt(node.Element("SlotType"), 0),
                Tier = ParseInt(node.Element("Tier"), -1),
                RateOfFire = ParseFloat(node.Element("RateOfFire"), 1f),
                NumProjectiles = ParseInt(node.Element("NumProjectiles"), 1),
                ArcGapDegrees = ParseFloat(node.Element("ArcGap"), 11.25f),
                Consumable = node.Element("Consumable") != null,
                ArtKey = o.ArtKey,
                Description = ((string)node.Element("Description"))?.Trim(),
            };
            item.BagType = ParseInt(node.Element("BagType"), 0);
            foreach (var boost in node.Elements("ActivateOnEquip"))
            {
                if (boost.Value.Trim() != "IncrementStat") continue;
                var name = StatName((int?)boost.Attribute("stat") ?? -1);
                if (name != null) item.Bonuses.Add(new KeyValuePair<string, int>(name, (int?)boost.Attribute("amount") ?? 0));
            }
            foreach (var act in node.Elements("Activate"))
            {
                var what = act.Value.Trim();
                if (what == "Heal") item.HealAmount = (int?)act.Attribute("amount") ?? 0;
                if (what == "Magic") item.MagicAmount = (int?)act.Attribute("amount") ?? 0;
            }
            var p = node.Element("Projectile");
            if (p != null)
            {
                var amplitude = ParseFloat(p.Element("Amplitude"), 0f);
                item.Projectile = new ProjectileDef
                {
                    ObjectId = ((string)p.Element("ObjectId") ?? "").Trim(),
                    Speed = ParseFloat(p.Element("Speed"), 0f) / 10f,
                    LifetimeMs = (int)ParseFloat(p.Element("LifetimeMS"), 0f),
                    Amplitude = amplitude,
                    Frequency = ParseFloat(p.Element("Frequency"), 1f),
                    Size = ParseInt(p.Element("Size"), 100),
                    // Same order as the server (reference ParsePath): Amplitude, Wavy, Boomerang, else Line.
                    Path = amplitude != 0f ? PathKind.Amplitude
                        : p.Element("Wavy") != null ? PathKind.Wavy
                        : p.Element("Boomerang") != null ? PathKind.Boomerang
                        : PathKind.Line,
                };
            }
            return item;
        }

        /// <summary>The reference's stat ids (Common Enumerables StatType), as the server maps them.</summary>
        public static string StatName(int id)
        {
            switch (id)
            {
                case 0: return "Max HP";
                case 3: return "Max MP";
                case 20: return "Attack";
                case 21: return "Defense";
                case 22: return "Speed";
                // The original's StatType numbers (alloy-server Common/Enumerables.cs), as the server reads them.
                case 26: return "Vitality";
                case 27: return "Wisdom";
                case 28: return "Dexterity";
                default: return null;
            }
        }
    }
}

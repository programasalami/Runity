using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Tilemaps;
using WaW.Domain.Content;
using WaW.Presentation.Art;
using WaW.Protocol;

namespace WaW.Presentation.World
{
    /// <summary>Draws received tiles like the original client: the ground on a Tilemap (one 8x8 cell per tile, a stable random pick for
    /// &lt;RandomTexture&gt; grounds), and every static object as an upright sprite standing on its tile. A wall shows its side texture
    /// on the tile and its &lt;Top&gt; texture one tile higher on screen, which is how the original's cube walls read at the default
    /// camera angle. Without art the tinted placeholders are drawn instead.</summary>
    public sealed class TileLayerView : MonoBehaviour
    {
        /// <summary>Statics farther than this from the camera are released (the server keeps sending what is near).</summary>
        public float KeepRadius = 28f;

        private Tilemap _ground;
        private ContentCatalog _content;
        private ArtCatalog _art;
        private Material _material;
        private Transform _statics;
        private readonly Dictionary<(ushort, int), Tile> _groundTiles = new Dictionary<(ushort, int), Tile>();
        private readonly Dictionary<Vector2Int, StaticObjectView> _objects = new Dictionary<Vector2Int, StaticObjectView>();
        private readonly Stack<StaticObjectView> _pool = new Stack<StaticObjectView>();
        private readonly List<Vector2Int> _drop = new List<Vector2Int>();

        public int TileCount { get; private set; }
        public int StaticCount => _objects.Count;

        public void Init(ContentCatalog content, ArtCatalog art, Material spriteMaterial)
        {
            _content = content;
            _art = art;
            _material = spriteMaterial;
            var grid = gameObject.AddComponent<Grid>();
            grid.cellSize = Vector3.one;
            var go = new GameObject("Ground");
            go.transform.SetParent(transform, false);
            _ground = go.AddComponent<Tilemap>();
            var r = go.AddComponent<TilemapRenderer>();
            r.sortingOrder = -32000;
            if (spriteMaterial != null) r.sharedMaterial = spriteMaterial;
            _statics = new GameObject("Statics").transform;
            _statics.SetParent(transform, false);
        }

        public void Clear()
        {
            _ground.ClearAllTiles();
            foreach (var v in _objects.Values) Release(v);
            _objects.Clear();
            TileCount = 0;
        }

        public void Apply(IReadOnlyList<TileUpdate> tiles)
        {
            foreach (var t in tiles)
            {
                var cell = Coordinates.TileCell(t.X, t.Y);
                if (_ground.GetTile(cell) == null) TileCount++;
                _ground.SetTile(cell, t.GroundType == 0xFF ? null : GroundTile(t.GroundType, t.X, t.Y));
                var key = new Vector2Int(t.X, t.Y);
                if (_objects.TryGetValue(key, out var old))
                {
                    if (old.ObjectType == t.ObjectType) continue;
                    Release(old);
                    _objects.Remove(key);
                }
                if (t.ObjectType == 0) continue;
                var def = _content.Object(t.ObjectType);
                var view = _pool.Count > 0 ? _pool.Pop() : StaticObjectView.Create(_statics, _material);
                view.Show(def, t.ObjectType, t.X, t.Y, _art);
                _objects[key] = view;
            }
        }

        /// <summary>Per frame: keeps the statics upright and in screen-depth order, and releases the far ones.</summary>
        public void Refresh(Vector2 cameraWorld, float cameraAngleDegrees)
        {
            var a = cameraAngleDegrees * Mathf.Deg2Rad;
            var keep = KeepRadius * KeepRadius;
            _drop.Clear();
            foreach (var kv in _objects)
            {
                var v = kv.Value;
                var dx = v.TileX + 0.5f - cameraWorld.x;
                var dy = v.TileY + 0.5f - cameraWorld.y;
                if (dx * dx + dy * dy > keep)
                {
                    _drop.Add(kv.Key);
                    continue;
                }
                v.Refresh(cameraAngleDegrees, Depth.Order(dx, dy, a));
            }
            foreach (var k in _drop)
            {
                Release(_objects[k]);
                _objects.Remove(k);
            }
        }

        private void Release(StaticObjectView v)
        {
            v.gameObject.SetActive(false);
            _pool.Push(v);
        }

        private Tile GroundTile(ushort type, int x, int y)
        {
            var def = _content.Ground(type);
            var variants = def?.RandomArtKeys;
            var variant = variants != null ? (int)(Hash(x, y) % (uint)variants.Count) : 0;
            if (_groundTiles.TryGetValue((type, variant), out var tile)) return tile;
            tile = ScriptableObject.CreateInstance<Tile>();
            var key = variants != null ? variants[variant] : def?.ArtKey;
            var sprite = _art != null ? _art.Find(key) : SheetAtlas.Shared?.Find(key);
            tile.sprite = sprite != null ? sprite : Placeholders.Square;
            tile.color = sprite != null ? Color.white
                : def == null ? new Color(0.05f, 0.05f, 0.07f)
                : def.NoWalk ? new Color(0.15f, 0.3f, 0.6f)
                : Placeholders.ColorFor(def.Id, 0.35f, 0.55f);
            _groundTiles[(type, variant)] = tile;
            return tile;
        }

        private static uint Hash(int x, int y)
        {
            unchecked
            {
                var h = (uint)(x * 73856093) ^ (uint)(y * 19349663);
                h ^= h >> 13;
                h *= 0x5bd1e995;
                return h ^ (h >> 15);
            }
        }
    }

    /// <summary>The original's draw order: farther up the screen draws first (depth from the screen-space y after the camera turn).</summary>
    public static class Depth
    {
        /// <summary>dx, dy: offset from the camera in tiles (y south); angle: camera angle in radians.</summary>
        public static int Order(float dx, float dy, float angle)
        {
            // Screen-down component of the offset once the camera has turned by angle.
            var down = dx * Mathf.Sin(angle) + dy * Mathf.Cos(angle);
            return Mathf.Clamp(Mathf.RoundToInt(down * 40f), -30000, 30000);
        }
    }

    /// <summary>One static object on a tile: an upright sprite with its feet on the tile centre (walls: side face plus the top one
    /// tile above it).</summary>
    public sealed class StaticObjectView : MonoBehaviour
    {
        public SpriteRenderer Body;
        public SpriteRenderer Top;
        public ushort ObjectType;
        public int TileX;
        public int TileY;
        private bool _wall;

        public static StaticObjectView Create(Transform parent, Material material)
        {
            var go = new GameObject("Static");
            go.transform.SetParent(parent, false);
            var v = go.AddComponent<StaticObjectView>();
            v.Body = Sprites.Make(go.transform, "Body", material);
            v.Top = Sprites.Make(go.transform, "Top", material);
            return v;
        }

        public void Show(ObjectDef def, ushort type, int x, int y, ArtCatalog art)
        {
            ObjectType = type;
            TileX = x;
            TileY = y;
            gameObject.SetActive(true);
            gameObject.name = def?.Id ?? "Static";
            _wall = def != null && (def.Class == "Wall" || def.Class == "ConnectedWall" || def.Class == "CaveWall" || def.FullOccupy && def.TopArtKey != null);
            var key = def?.RandomArtKeys != null ? def.RandomArtKeys[(x * 31 + y * 17 & 0x7fffffff) % def.RandomArtKeys.Count] : def?.ArtKey;
            var sprite = Sprites.Find(art, key);
            transform.position = Coordinates.ToUnity(x + 0.5f, y + 0.5f, 0f);
            if (_wall)
            {
                // A unit cube seen from the south: the side face fills the tile, the top sits one tile up on screen.
                Body.sprite = sprite != null ? sprite : Placeholders.Square;
                Body.color = sprite != null ? new Color(0.86f, 0.86f, 0.86f) : new Color(0.3f, 0.28f, 0.26f);
                Sprites.FitTile(Body, 1f);
                Body.transform.localPosition = Vector3.zero;  // south face: from the footprint's south edge up to the top face
                var top = Sprites.Find(art, def.TopArtKey) ?? sprite;
                Top.enabled = true;
                Top.sprite = top != null ? top : Placeholders.Square;
                Top.color = top != null ? Color.white : new Color(0.42f, 0.4f, 0.37f);
                Sprites.FitTile(Top, 1f);
                Top.transform.localPosition = new Vector3(0, 1f, 0);  // the top at height 1 shows one tile up (oblique view)
                return;
            }
            Top.enabled = false;
            if (sprite == null)
            {
                Body.sprite = Placeholders.Disc;
                Body.color = Placeholders.ColorFor(def?.Id, 0.5f, 0.45f);
                Body.transform.localScale = Vector3.one * 0.6f;
                Body.transform.localPosition = new Vector3(0, 0.3f, 0);
                return;
            }
            Body.sprite = sprite;
            Body.color = Color.white;
            Sprites.FitCharacter(Body, sprite, def?.Size ?? 100, centred: false);
        }

        public void Refresh(float cameraAngleDegrees, int order)
        {
            // Upright on screen whatever the camera angle (walls too: a simplification of the original's 3D cubes).
            transform.rotation = Quaternion.Euler(0, 0, cameraAngleDegrees);
            Body.sortingOrder = order;
            Top.sortingOrder = order + 1;
        }
    }

    /// <summary>Sprite helpers shared by tiles, statics and entities, with the original's sizes.</summary>
    public static class Sprites
    {
        /// <summary>One texel of an 8 px cell is 0.0975 tile at Size 100 (an 8x8 sprite is ~0.78 tiles tall).</summary>
        public const float TileScalePerTexel = 0.0975f;

        public static SpriteRenderer Make(Transform parent, string name, Material material)
        {
            var go = new GameObject(name);
            go.transform.SetParent(parent, false);
            var r = go.AddComponent<SpriteRenderer>();
            if (material != null) r.sharedMaterial = material;
            return r;
        }

        public static Sprite Find(ArtCatalog art, string key) =>
            string.IsNullOrEmpty(key) ? null : art != null ? art.Find(key) : SheetAtlas.Shared?.Find(key);

        /// <summary>Scales a sprite to cover size x size tiles (walls, flat things).</summary>
        public static void FitTile(SpriteRenderer r, float size)
        {
            var s = r.sprite;
            var h = s != null ? s.rect.height / s.pixelsPerUnit : 1f;
            var w = s != null ? s.rect.width / s.pixelsPerUnit : 1f;
            r.transform.localScale = new Vector3(size / w, size / h, 1f);
        }

        /// <summary>The original billboard size: texel height x 0.0975 x Size/100, feet on the anchor (centred for projectiles).</summary>
        public static void FitCharacter(SpriteRenderer r, Sprite sprite, int size, bool centred)
        {
            var heightTiles = sprite.rect.height * TileScalePerTexel * size / 100f;
            var unitsHigh = sprite.rect.height / sprite.pixelsPerUnit;
            var k = heightTiles / unitsHigh;
            r.transform.localScale = new Vector3(k, k, 1f);
            // Sheet cells have their pivot at the centre (static) or bottom (animated); stand static ones on their feet.
            var pivotFromBottom = sprite.pivot.y / sprite.pixelsPerUnit;
            r.transform.localPosition = centred ? Vector3.zero : new Vector3(0f, pivotFromBottom * k, 0f);
        }
    }
}

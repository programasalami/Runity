using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Tilemaps;
using Runity.Domain.Content;
using Runity.Presentation.Art;
using Runity.Protocol;

namespace Runity.Presentation.World
{
    /// <summary>Draws received tiles like the original client: the ground on a Tilemap (one 8x8 cell per tile, a stable random pick for
    /// &lt;RandomTexture&gt; grounds), every static object as an upright sprite standing on its tile, and walls as the original's cubes
    /// (WallSides). Without art the tinted placeholders are drawn instead.</summary>
    public sealed class TileLayerView : MonoBehaviour
    {
        /// <summary>Static object views farther than this from the camera are released. The server sends each tile only once per visit,
        /// so what was received stays known and its view is made again when the camera comes back within ShowRadius.</summary>
        public float KeepRadius = 28f;
        public float ShowRadius = 26f;  // a little inside KeepRadius, so a view at the edge is not made and released over and over

        private Tilemap _ground;
        private ContentCatalog _content;
        private ArtCatalog _art;
        private Material _material;
        private Transform _statics;
        private WallSides _walls;
        private readonly Dictionary<(ushort, int), Tile> _groundTiles = new Dictionary<(ushort, int), Tile>();
        private readonly Dictionary<Vector2Int, ushort> _known = new Dictionary<Vector2Int, ushort>();
        private readonly Dictionary<Vector2Int, StaticObjectView> _objects = new Dictionary<Vector2Int, StaticObjectView>();
        private readonly Stack<StaticObjectView> _pool = new Stack<StaticObjectView>();
        private readonly List<Vector2Int> _drop = new List<Vector2Int>();
        private Vector2Int _cameraTile = new Vector2Int(int.MinValue, int.MinValue);

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
            _walls = new WallSides();
        }

        public void Clear()
        {
            _ground.ClearAllTiles();
            foreach (var v in _objects.Values) Release(v);
            _objects.Clear();
            _known.Clear();
            _cameraTile = new Vector2Int(int.MinValue, int.MinValue);
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
                if (t.ObjectType == 0) _known.Remove(key);
                else _known[key] = t.ObjectType;
                if (_objects.TryGetValue(key, out var old))
                {
                    if (old.ObjectType == t.ObjectType) continue;
                    Release(old);
                    _objects.Remove(key);
                }
                if (t.ObjectType != 0) Show(key, t.ObjectType);
            }
        }

        /// <summary>Per frame: keeps the statics upright (walls: turned with the ground) and in screen-depth order, and releases the far
        /// ones.</summary>
        public void Refresh(Vector2 cameraWorld, float cameraAngleDegrees)
        {
            _walls.SetCameraAngle(cameraAngleDegrees);
            var cameraTile = new Vector2Int(Mathf.FloorToInt(cameraWorld.x), Mathf.FloorToInt(cameraWorld.y));
            if (cameraTile != _cameraTile)
            {
                _cameraTile = cameraTile;
                ShowKnownAround(cameraTile);
            }
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

        /// <summary>Makes the views of known statics near the camera that have none (released while the camera was away).</summary>
        private void ShowKnownAround(Vector2Int centre)
        {
            var reach = Mathf.CeilToInt(ShowRadius);
            var show = ShowRadius * ShowRadius;
            for (var y = -reach; y <= reach; y++)
            {
                for (var x = -reach; x <= reach; x++)
                {
                    if (x * x + y * y > show) continue;
                    var key = new Vector2Int(centre.x + x, centre.y + y);
                    if (!_objects.ContainsKey(key) && _known.TryGetValue(key, out var type)) Show(key, type);
                }
            }
        }

        private void Show(Vector2Int tile, ushort type)
        {
            var view = _pool.Count > 0 ? _pool.Pop() : StaticObjectView.Create(_statics, _material, _walls);
            view.Show(_content.Object(type), type, tile.x, tile.y, _art);
            _objects[tile] = view;
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
            var sprite = Sprites.FindTile(_art, key);
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

    /// <summary>The side faces every wall shares at the current camera angle. The original draws walls as unit cubes in an oblique view
    /// where height goes straight up the screen: the base square on the tile, the top square one tile up the screen, and between them
    /// the sides that face the viewer. The cube turns with the ground (its squares stay on the tile grid), so turning the camera never
    /// opens gaps around it. Every wall has the same shape, so one mesh serves them all.</summary>
    public sealed class WallSides
    {
        // The base square around the tile centre, counter-clockwise: each edge runs left to right seen from outside its face.
        private static readonly Vector3[] Corners =
        {
            new Vector3(-0.5f, -0.5f, 0f), new Vector3(0.5f, -0.5f, 0f), new Vector3(0.5f, 0.5f, 0f), new Vector3(-0.5f, 0.5f, 0f),
        };
        private static readonly Color SideShade = new Color(0.86f, 0.86f, 0.86f, 1f);

        // A mesh needs URP's 2D mesh shader: the sprite shader takes its colour from a SpriteRenderer and draws a mesh invisible.
        // It is in the build through Project Settings > Graphics > Always Included Shaders.
        private const string MeshShader = "Universal Render Pipeline/2D/Mesh2D-Unlit-Default";

        private readonly Shader _shader = Shader.Find(MeshShader);
        private readonly Dictionary<Texture, Material> _materials = new Dictionary<Texture, Material>();
        private readonly List<Vector3> _vertices = new List<Vector3>(8);
        private readonly List<Vector2> _uvs = new List<Vector2>(8);
        private readonly List<int> _triangles = new List<int>(12);
        private float _angle = float.NaN;

        public WallSides()
        {
            if (_shader == null) Debug.LogError($"[Runity] shader '{MeshShader}' is not in the build: walls have no sides");
            Mesh = new Mesh { name = "Wall sides" };
            Mesh.MarkDynamic();
        }

        public Mesh Mesh { get; }

        /// <summary>One tile of height, in world units: straight up the screen.</summary>
        public Vector3 Up { get; private set; } = Vector3.up;

        /// <summary>The material showing one side picture, shaded (a picture on its own texture, see SheetAtlas.Tile).</summary>
        public Material MaterialFor(Texture texture)
        {
            if (_materials.TryGetValue(texture, out var m)) return m;
            m = new Material(_shader) { mainTexture = texture, name = "Wall side " + texture.name };
            m.SetColor("_White", SideShade);  // the shader's tint
            _materials[texture] = m;
            return m;
        }

        public void SetCameraAngle(float degrees)
        {
            if (degrees == _angle) return;
            _angle = degrees;
            Up = Quaternion.Euler(0f, 0f, degrees) * Vector3.up;
            _vertices.Clear();
            _uvs.Clear();
            _triangles.Clear();
            for (var i = 0; i < Corners.Length; i++)
            {
                var a = Corners[i];
                var b = Corners[(i + 1) % Corners.Length];
                var outward = new Vector3(b.y - a.y, a.x - b.x, 0f);
                if (Vector3.Dot(outward, Up) > -0.001f) continue;  // faces up the screen (hidden behind the top) or edge-on
                var n = _vertices.Count;
                _vertices.Add(a);
                _vertices.Add(b);
                _vertices.Add(b + Up);
                _vertices.Add(a + Up);
                _uvs.Add(new Vector2(0f, 0f));
                _uvs.Add(new Vector2(1f, 0f));
                _uvs.Add(new Vector2(1f, 1f));
                _uvs.Add(new Vector2(0f, 1f));
                // Clockwise on screen (the shader culls back faces).
                _triangles.Add(n);
                _triangles.Add(n + 2);
                _triangles.Add(n + 1);
                _triangles.Add(n);
                _triangles.Add(n + 3);
                _triangles.Add(n + 2);
            }
            Mesh.Clear();
            Mesh.SetVertices(_vertices);
            Mesh.SetUVs(0, _uvs);
            Mesh.SetTriangles(_triangles, 0);
            Mesh.RecalculateBounds();
        }
    }

    /// <summary>One static object on a tile: an upright sprite with its feet on the tile centre, or a wall cube (the shared sides plus
    /// its top).</summary>
    public sealed class StaticObjectView : MonoBehaviour
    {
        public SpriteRenderer Body;
        public SpriteRenderer Top;
        public MeshRenderer Sides;
        public ushort ObjectType;
        public int TileX;
        public int TileY;
        private WallSides _walls;
        private bool _wall;

        public static StaticObjectView Create(Transform parent, Material material, WallSides walls)
        {
            var go = new GameObject("Static");
            go.transform.SetParent(parent, false);
            var v = go.AddComponent<StaticObjectView>();
            v._walls = walls;
            v.Body = Sprites.Make(go.transform, "Body", material);
            v.Top = Sprites.Make(go.transform, "Top", material);
            var sides = new GameObject("Sides", typeof(MeshFilter), typeof(MeshRenderer));
            sides.transform.SetParent(go.transform, false);
            sides.GetComponent<MeshFilter>().sharedMesh = walls.Mesh;
            v.Sides = sides.GetComponent<MeshRenderer>();
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
            transform.position = Coordinates.ToUnity(x + 0.5f, y + 0.5f, 0f);
            transform.rotation = Quaternion.identity;
            if (_wall)
            {
                var side = Sprites.FindTile(art, key);
                Body.enabled = false;
                Sides.enabled = true;
                Sides.sharedMaterial = _walls.MaterialFor((side != null ? side : Placeholders.Square).texture);
                var top = Sprites.FindTile(art, def.TopArtKey) ?? side;
                Top.enabled = true;
                Top.sprite = top != null ? top : Placeholders.Square;
                Top.color = top != null ? Color.white : new Color(0.42f, 0.4f, 0.37f);
                Sprites.FitTile(Top, 1f);
                return;
            }
            Sides.enabled = false;
            Top.enabled = false;
            Body.enabled = true;
            var sprite = Sprites.Find(art, key);
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
            if (_wall)
            {
                // The cube stays on the tile grid; its top sits one tile up the screen, whatever the camera angle.
                Top.transform.localPosition = _walls.Up;
                Sides.sortingOrder = order;
                Top.sortingOrder = order + 1;
                return;
            }
            transform.rotation = Quaternion.Euler(0, 0, cameraAngleDegrees);  // upright on screen
            Body.sortingOrder = order;
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

        /// <summary>A ground or wall picture on its own texture (SheetAtlas.Tile): no dark seams between tiles.</summary>
        public static Sprite FindTile(ArtCatalog art, string key) =>
            string.IsNullOrEmpty(key) ? null : art != null ? art.FindTile(key) : SheetAtlas.Shared?.Tile(key);

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

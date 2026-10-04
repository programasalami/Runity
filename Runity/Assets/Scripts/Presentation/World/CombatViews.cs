using System.Collections.Generic;
using UnityEngine;
using Runity.Domain.Content;
using Runity.Domain.World;
using Runity.Presentation.Art;
using Runity.Protocol;

namespace Runity.Presentation.World
{
    /// <summary>Draws the client's projectiles (ClientWorld.Projectiles) with pooled sprites, like the original: the projectile
    /// object's sprite centred on the bullet half a tile above the ground, sized like characters x Size/100, pointing along its
    /// flight (+ AngleCorrection eighths of a turn) or spinning when the object has a &lt;Rotation&gt;.</summary>
    public sealed class ProjectileViews : MonoBehaviour
    {
        private readonly Dictionary<ClientProjectile, SpriteRenderer> _views = new Dictionary<ClientProjectile, SpriteRenderer>();
        private readonly Stack<SpriteRenderer> _pool = new Stack<SpriteRenderer>();
        private readonly Dictionary<SpriteRenderer, ObjectDef> _defs = new Dictionary<SpriteRenderer, ObjectDef>();
        private ClientProjectiles _source;
        private ContentCatalog _content;
        private ArtCatalog _art;
        private Material _material;

        public int Count => _views.Count;

        public void Init(ContentCatalog content, ArtCatalog art, Material material)
        {
            _content = content;
            _art = art;
            _material = material;
        }

        public void Attach(ClientProjectiles source)
        {
            Detach();
            _source = source;
            _source.Spawned += OnSpawned;
            _source.Ended += OnEnded;
        }

        public void Detach()
        {
            if (_source == null) return;
            _source.Spawned -= OnSpawned;
            _source.Ended -= OnEnded;
            foreach (var r in _views.Values) Release(r);
            _views.Clear();
            _source = null;
        }

        private void OnSpawned(ClientProjectile p)
        {
            var r = _pool.Count > 0 ? _pool.Pop() : Create();
            r.gameObject.SetActive(true);
            var def = _content.Object(p.ProjectileType);
            var sprite = Sprites.Find(_art, def?.ArtKey);
            if (sprite != null)
            {
                r.sprite = sprite;
                r.color = Color.white;
                Sprites.FitCharacter(r, sprite, p.Size, centred: true);
            }
            else
            {
                r.sprite = Placeholders.Disc;
                r.color = p.Enemy ? new Color(1f, 0.45f, 0.2f) : new Color(0.55f, 0.9f, 1f);
                r.transform.localScale = Vector3.one * Mathf.Clamp(p.Size / 100f * 0.45f, 0.15f, 1.5f);
            }
            _defs[r] = def;
            _views[p] = r;
            Place(p, r, def);
        }

        private void OnEnded(ClientProjectile p)
        {
            if (!_views.TryGetValue(p, out var r)) return;
            _views.Remove(p);
            Release(r);
        }

        private void Release(SpriteRenderer r)
        {
            r.gameObject.SetActive(false);
            _pool.Push(r);
        }

        private SpriteRenderer Create()
        {
            var go = new GameObject("Projectile");
            go.transform.SetParent(transform, false);
            var r = go.AddComponent<SpriteRenderer>();
            r.sortingOrder = 20000;
            if (_material != null) r.sharedMaterial = _material;
            return r;
        }

        private static void Place(ClientProjectile p, SpriteRenderer r, ObjectDef def)
        {
            // Drawn at height 0.5: half a tile up on screen whatever the camera angle.
            var up = Camera.main != null ? Camera.main.transform.up : Vector3.up;
            r.transform.position = Coordinates.ToUnity(p.Position.X, p.Position.Y, -0.2f) + up * 0.5f;
            float radians;
            if (def != null && def.Rotation != 0f) radians = p.ElapsedMs / def.Rotation;
            else
            {
                var dx = p.Position.X - p.Previous.X;
                var dy = p.Position.Y - p.Previous.Y;
                var heading = dx * dx + dy * dy > 1e-10f ? Mathf.Atan2(dy, dx) : p.Angle;
                radians = heading + (def?.AngleCorrection ?? 0) * Mathf.PI / 4f;
            }
            // World angles have y south; Unity's z rotation is counter-clockwise with y up.
            r.transform.rotation = Quaternion.Euler(0, 0, -radians * Mathf.Rad2Deg);
        }

        private void LateUpdate()
        {
            foreach (var pair in _views) Place(pair.Key, pair.Value, _defs.TryGetValue(pair.Value, out var d) ? d : null);
        }
    }

    /// <summary>Floating numbers and words over entities (damage, XP, heals, level-ups): rise, then fade.</summary>
    public sealed class FloatingTexts : MonoBehaviour
    {
        private sealed class Item
        {
            public TextMesh Text;
            public uint EntityId;
            public float Age;
            public Color Color;
        }

        private const float LifeSeconds = 1.1f;
        private readonly List<Item> _live = new List<Item>();
        private readonly Stack<Item> _pool = new Stack<Item>();
        private Font _font;
        private ClientWorld _world;
        private float _cameraAngleDegrees;

        public void Init(Font font) => _font = font;

        public void Attach(ClientWorld world)
        {
            Detach();
            _world = world;
            _world.NotificationReceived += OnNotification;
        }

        public void Detach()
        {
            if (_world != null) _world.NotificationReceived -= OnNotification;
            _world = null;
            foreach (var i in _live) Release(i);
            _live.Clear();
        }

        public void SetCameraAngle(float degrees) => _cameraAngleDegrees = degrees;

        private void OnNotification(Notification n)
        {
            var item = _pool.Count > 0 ? _pool.Pop() : Create();
            item.Text.gameObject.SetActive(true);
            item.Text.text = n.Text;
            item.EntityId = n.EntityId;
            item.Age = 0f;
            item.Color = new Color32((byte)(n.Color >> 24), (byte)(n.Color >> 16), (byte)(n.Color >> 8), 255);
            item.Text.color = item.Color;
            item.Text.fontSize = n.IsDamage ? 52 : 44;
            _live.Add(item);
        }

        private Item Create()
        {
            var go = new GameObject("FloatingText");
            go.transform.SetParent(transform, false);
            var t = go.AddComponent<TextMesh>();
            t.font = _font;
            t.characterSize = 0.05f;
            t.anchor = TextAnchor.LowerCenter;
            t.alignment = TextAlignment.Center;
            var r = go.GetComponent<MeshRenderer>();
            r.sharedMaterial = _font.material;
            r.sortingOrder = 40;
            return new Item { Text = t };
        }

        private void Release(Item i)
        {
            i.Text.gameObject.SetActive(false);
            _pool.Push(i);
        }

        private void LateUpdate()
        {
            for (var k = _live.Count - 1; k >= 0; k--)
            {
                var i = _live[k];
                i.Age += Time.deltaTime;
                var e = _world?.Find(i.EntityId);
                if (i.Age >= LifeSeconds || e == null)
                {
                    Release(i);
                    _live.RemoveAt(k);
                    continue;
                }
                // Rise straight up on screen, whatever the camera's turn.
                var up = Quaternion.Euler(0, 0, _cameraAngleDegrees) * Vector3.up;
                i.Text.transform.position = Coordinates.ToUnity(e.RenderPosition.X, e.RenderPosition.Y, -0.3f) + up * (0.9f + i.Age * 0.9f);
                i.Text.transform.rotation = Quaternion.Euler(0, 0, _cameraAngleDegrees);
                var c = i.Color;
                c.a = 1f - Mathf.Clamp01((i.Age - 0.6f) / (LifeSeconds - 0.6f));
                i.Text.color = c;
            }
        }
    }
}

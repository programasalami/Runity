using System.Collections.Generic;
using UnityEngine;
using Runity.Domain.Content;
using Runity.Domain.World;
using Runity.Presentation.Art;
using Runity.Protocol;

namespace Runity.Presentation.World
{
    /// <summary>The look of one entity, as the original client draws it (TypeGameObject / TypePlayer): an upright sprite with its feet
    /// on the position, an elliptical shadow, the HP bar under the feet and, for other players, a gold name. Characters with an
    /// &lt;AnimatedTexture&gt; face the way they move or shoot, walk with frames 1-2 and attack with frames 4-5. It only copies state
    /// from its ClientEntity; it never decides anything.</summary>
    public sealed class EntityView : MonoBehaviour
    {
        public const float EntityAttackPeriodMs = 300f;

        public SpriteRenderer Body;
        public SpriteRenderer Shadow;
        public TextMesh Label;
        public SpriteRenderer HpBack;
        public SpriteRenderer HpFill;
        [System.NonSerialized] public ClientEntity Entity;

        private ObjectDef _def;
        private ArtCatalog _art;
        private bool _isLocal;
        private string _sheet;
        private int _index;
        private Domain.World.Vec2 _last;
        private float _moveAngle;
        private float _speed;
        private float _attackStartMs = -1e9f;
        private float _attackAngle;
        private float _attackPeriodMs = EntityAttackPeriodMs;
        private float _clockMs;

        public void Bind(ClientEntity entity, ContentCatalog content, ArtCatalog art, bool isLocal)
        {
            Entity = entity;
            _art = art;
            _isLocal = isLocal;
            _def = content.Object(entity.ObjectType);
            _sheet = null;
            if (_def != null && _def.Animated && _def.ArtKey != null)
            {
                var colon = _def.ArtKey.LastIndexOf(':');
                _sheet = _def.ArtKey.Substring(0, colon);
                var idx = _def.ArtKey.Substring(colon + 1);
                _index = idx.StartsWith("0x") ? System.Convert.ToInt32(idx.Substring(2), 16) : int.Parse(idx);
            }
            _last = entity.RenderPosition;
            _attackStartMs = -1e9f;
            var sprite = Sprites.Find(art, _def?.ArtKey);
            if (sprite != null)
            {
                Body.sprite = sprite;
                Body.color = Color.white;
            }
            else
            {
                Body.sprite = Placeholders.Disc;
                Body.color = isLocal ? new Color(1f, 0.85f, 0.3f)
                    : entity.Kind == EntityKind.Player ? new Color(0.35f, 0.75f, 1f)
                    : entity.Kind == EntityKind.Enemy ? new Color(0.9f, 0.3f, 0.25f)
                    : entity.Kind == EntityKind.Portal ? new Color(0.7f, 0.4f, 1f)
                    : Placeholders.ColorFor(_def?.Id, 0.5f, 0.8f);
            }
            ApplySize();
            // Name tags: other players only (gold 0xFCDF00), never the local player or monsters.
            var showName = entity.Kind == EntityKind.Player && !isLocal;
            Label.gameObject.SetActive(showName);
            Label.text = showName ? entity.Name : "";
            gameObject.name = $"Entity {entity.Id} {(string.IsNullOrEmpty(entity.Name) ? _def?.Id : entity.Name)}";
            Shadow.enabled = (_def?.Size ?? 100) > 0;
            Refresh(0f, 0f, 0f);
        }

        /// <summary>The entity fired (a volley left it): show the attack frames towards angle (world radians).</summary>
        public void Attack(float angle, float periodMs)
        {
            _attackStartMs = _clockMs;
            _attackAngle = angle;
            _attackPeriodMs = periodMs > 0 ? periodMs : EntityAttackPeriodMs;
        }

        public void Refresh(float cameraAngleDegrees, float deltaMs, float moveSpeedTilesPerMs)
        {
            _clockMs += deltaMs;
            var p = Entity.RenderPosition;
            transform.position = Coordinates.ToUnity(p.X, p.Y, 0f);
            transform.rotation = Quaternion.Euler(0, 0, cameraAngleDegrees);

            var dx = p.X - _last.X;
            var dy = p.Y - _last.Y;
            var moved = dx * dx + dy * dy;
            _speed = deltaMs > 0 ? Mathf.Sqrt(moved) / deltaMs : 0f;
            if (moved > 1e-8f) _moveAngle = Mathf.Atan2(dy, dx);
            _last = p;
            if (_sheet != null) Animate(-cameraAngleDegrees * Mathf.Deg2Rad, moveSpeedTilesPerMs);

            var showHp = Entity.MaxHp > 0 && (Entity.Kind == EntityKind.Player || Entity.Kind == EntityKind.Enemy);
            HpBack.enabled = HpFill.enabled = showHp;
            if (showHp)
            {
                var f = Mathf.Clamp01(Entity.Hp / (float)Entity.MaxHp);
                // Original colours: green >= 50 %, orange 20-50 %, red below; the fill is left-anchored (Alloy's offset bug fixed).
                HpFill.color = f >= 0.5f ? new Color32(0x10, 0xFF, 0x00, 0xFF) : f >= 0.2f ? new Color32(0xFF, 0x80, 0x10, 0xFF) : new Color32(0xE0, 0x10, 0x10, 0xFF);
                HpFill.transform.localScale = new Vector3(0.68f * f, 0.08f, 1f);
                HpFill.transform.localPosition = new Vector3(-0.34f + 0.34f * f, BarY, 0f);
            }
        }

        private float BarY => Label.gameObject.activeSelf ? -0.42f : -0.16f;

        private void Animate(float cameraAngle, float localMoveSpeed)
        {
            var attacking = _clockMs - _attackStartMs < _attackPeriodMs;
            var angle = attacking ? _attackAngle : _moveAngle;
            // TextureHelper.TextureFromFacing: 8 sectors relative to the camera.
            var rel = Mathf.Repeat(angle - cameraAngle + Mathf.PI, Mathf.PI * 2f) - Mathf.PI;
            var sec = (int)(rel / (Mathf.PI / 4f) + 4f) % 8;
            Facing facing;
            var flip = false;
            switch (sec)
            {
                case 0:
                case 7: facing = Facing.Right; flip = true; break;
                case 1:
                case 2: facing = Facing.Up; break;
                case 5:
                case 6: facing = Facing.Down; break;
                default: facing = Facing.Right; break;
            }
            int frame;
            if (attacking)
            {
                var idx = (_clockMs - _attackStartMs) % _attackPeriodMs / _attackPeriodMs;
                frame = SheetAtlas.AttackFrame + (int)(idx * 2f);
            }
            else if (_speed > 0.0004f)
            {
                var walkPeriod = _isLocal && localMoveSpeed > 0 ? 3.5f / localMoveSpeed : Mathf.Ceil(0.5f / (_speed * 4f) / 400f) * 400f;
                var idx = _clockMs % walkPeriod / walkPeriod;
                frame = 1 + (int)(idx * 2f);
            }
            else
            {
                frame = 0;
            }
            var sprite = SheetAtlas.Shared?.Frame(_sheet, _index, facing, frame) ?? SheetAtlas.Shared?.Frame(_sheet, _index, facing, 0);
            if (sprite == null) return;
            if (Body.sprite != sprite)
            {
                Body.sprite = sprite;
                ApplySize();
            }
            Body.flipX = flip;
        }

        private void ApplySize()
        {
            if (Body.sprite == null) return;
            if (Body.sprite == Placeholders.Disc)
            {
                var size = Entity.Kind == EntityKind.Player ? 0.8f : Mathf.Clamp((_def?.Size ?? 100) / 100f * 0.8f, 0.3f, 3f);
                Body.transform.localScale = Vector3.one * size;
                Body.transform.localPosition = new Vector3(0, size * 0.5f, 0);
                return;
            }
            Sprites.FitCharacter(Body, Body.sprite, _def?.Size ?? 100, centred: false);
        }

        public void SetOrder(int order)
        {
            Shadow.sortingOrder = -31000;
            Body.sortingOrder = order;
            HpBack.sortingOrder = order + 1;
            HpFill.sortingOrder = order + 2;
            Label.GetComponent<MeshRenderer>().sortingOrder = 30000;
        }
    }

    /// <summary>Creates, pools and updates one EntityView per ClientEntity, keyed by entity id.</summary>
    public sealed class EntityViewRegistry : MonoBehaviour
    {
        private readonly Dictionary<uint, EntityView> _views = new Dictionary<uint, EntityView>();
        private readonly Stack<EntityView> _pool = new Stack<EntityView>();
        private ContentCatalog _content;
        private ArtCatalog _art;
        private Material _material;
        private Font _font;
        private static Sprite _shadow;

        public int Count => _views.Count;
        public IReadOnlyDictionary<uint, EntityView> Views => _views;

        public void Init(ContentCatalog content, ArtCatalog art, Material material, Font font)
        {
            _content = content;
            _art = art;
            _material = material;
            _font = font;
        }

        public void Add(ClientEntity entity, bool isLocal)
        {
            if (_views.ContainsKey(entity.Id)) Remove(entity.Id);
            var view = _pool.Count > 0 ? _pool.Pop() : Create();
            view.gameObject.SetActive(true);
            view.Bind(entity, _content, _art, isLocal);
            _views[entity.Id] = view;
        }

        public void Remove(uint id)
        {
            if (!_views.TryGetValue(id, out var view)) return;
            _views.Remove(id);
            view.Entity = null;
            view.gameObject.SetActive(false);
            _pool.Push(view);
        }

        public void Clear()
        {
            foreach (var id in new List<uint>(_views.Keys)) Remove(id);
        }

        public void Attack(uint entityId, float angle, float periodMs = 0)
        {
            if (_views.TryGetValue(entityId, out var v)) v.Attack(angle, periodMs);
        }

        public void Refresh(float cameraAngleDegrees, Vector2 cameraWorld, float deltaMs, float localMoveSpeed)
        {
            var a = -cameraAngleDegrees * Mathf.Deg2Rad;
            foreach (var v in _views.Values)
            {
                v.Refresh(cameraAngleDegrees, deltaMs, localMoveSpeed);
                var p = v.Entity.RenderPosition;
                v.SetOrder(Depth.Order(p.X - cameraWorld.x, p.Y - cameraWorld.y, a) + 2);
            }
        }

        private EntityView Create()
        {
            var go = new GameObject("Entity");
            go.transform.SetParent(transform, false);
            var view = go.AddComponent<EntityView>();
            view.Shadow = Sprites.Make(go.transform, "Shadow", _material);
            view.Shadow.sprite = ShadowSprite();
            view.Shadow.color = Color.white;
            view.Shadow.transform.localScale = new Vector3(1f, 0.5f, 1f);
            view.Body = Sprites.Make(go.transform, "Body", _material);
            view.HpBack = Sprites.Make(go.transform, "HpBack", _material);
            view.HpBack.sprite = Placeholders.Square;
            view.HpBack.color = new Color32(0x11, 0x11, 0x11, 0xFF);
            view.HpBack.transform.localScale = new Vector3(0.72f, 0.12f, 1f);
            view.HpBack.transform.localPosition = new Vector3(0f, -0.16f, 0f);
            view.HpFill = Sprites.Make(go.transform, "HpFill", _material);
            view.HpFill.sprite = Placeholders.Square;

            var label = new GameObject("Name");
            label.transform.SetParent(go.transform, false);
            label.transform.localPosition = new Vector3(0f, -0.1f, 0f);
            var text = label.AddComponent<TextMesh>();
            text.font = _font;
            text.fontSize = 48;
            text.characterSize = 0.045f;
            text.anchor = TextAnchor.UpperCenter;
            text.alignment = TextAlignment.Center;
            text.color = new Color32(0xFC, 0xDF, 0x00, 0xFF);
            label.GetComponent<MeshRenderer>().sharedMaterial = _font.material;
            view.Label = text;
            return view;
        }

        /// <summary>The original's shadow: a black ellipse, alpha (0.25 - r^2) * 1.5 from the centre out.</summary>
        private static Sprite ShadowSprite()
        {
            if (_shadow != null) return _shadow;
            const int n = 32;
            var tex = new Texture2D(n, n, TextureFormat.RGBA32, false) { wrapMode = TextureWrapMode.Clamp, name = "Shadow" };
            var px = new Color32[n * n];
            for (var y = 0; y < n; y++)
            {
                for (var x = 0; x < n; x++)
                {
                    var dx = (x + 0.5f) / n - 0.5f;
                    var dy = (y + 0.5f) / n - 0.5f;
                    var a = Mathf.Clamp01((0.25f - (dx * dx + dy * dy)) * 1.5f);
                    px[y * n + x] = new Color32(0, 0, 0, (byte)(a * 255));
                }
            }
            tex.SetPixels32(px);
            tex.Apply();
            return _shadow = Sprite.Create(tex, new Rect(0, 0, n, n), new Vector2(0.5f, 0.5f), n);
        }
    }
}

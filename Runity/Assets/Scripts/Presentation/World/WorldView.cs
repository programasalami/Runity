using UnityEngine;
using Runity.Domain.World;
using Runity.Presentation.Art;
using Runity.Protocol;

namespace Runity.Presentation.World
{
    /// <summary>Connects one ClientWorld to its Unity representation: tiles, entity views and the camera. It listens to the world's
    /// events and copies state every frame; it holds no game state of its own.</summary>
    public sealed class WorldView : MonoBehaviour
    {
        public TileLayerView Tiles { get; private set; }
        public EntityViewRegistry Entities { get; private set; }
        public CameraRig CameraRig { get; private set; }
        public ProjectileViews Projectiles { get; private set; }
        public FloatingTexts Texts { get; private set; }
        private ClientWorld _world;

        public void Init(Camera cam, ArtCatalog art, Material spriteMaterial, Font font, Domain.Content.ContentCatalog content)
        {
            var tiles = new GameObject("Tiles");
            tiles.transform.SetParent(transform, false);
            Tiles = tiles.AddComponent<TileLayerView>();
            Tiles.Init(content, art, spriteMaterial);

            var entities = new GameObject("Entities");
            entities.transform.SetParent(transform, false);
            Entities = entities.AddComponent<EntityViewRegistry>();
            Entities.Init(content, art, spriteMaterial, font);

            var projectiles = new GameObject("Projectiles");
            projectiles.transform.SetParent(transform, false);
            Projectiles = projectiles.AddComponent<ProjectileViews>();
            Projectiles.Init(content, art, spriteMaterial);

            var texts = new GameObject("FloatingTexts");
            texts.transform.SetParent(transform, false);
            Texts = texts.AddComponent<FloatingTexts>();
            Texts.Init(font);

            CameraRig = gameObject.AddComponent<CameraRig>();
            CameraRig.Init(cam);
        }

        public void Attach(ClientWorld world)
        {
            Detach();
            _world = world;
            _world.WorldChanged += OnWorldChanged;
            _world.TilesReceived += Tiles.Apply;
            _world.EntityAdded += OnEntityAdded;
            _world.EntityRemoved += OnEntityRemoved;
            Projectiles.Attach(_world.Projectiles);
            _world.Projectiles.Spawned += OnProjectileSpawned;
            Texts.Attach(_world);
        }

        public void Detach()
        {
            if (_world == null) return;
            _world.WorldChanged -= OnWorldChanged;
            _world.TilesReceived -= Tiles.Apply;
            _world.EntityAdded -= OnEntityAdded;
            _world.EntityRemoved -= OnEntityRemoved;
            _world.Projectiles.Spawned -= OnProjectileSpawned;
            Projectiles.Detach();
            Texts.Detach();
            _world = null;
            Tiles.Clear();
            Entities.Clear();
        }

        private void OnWorldChanged(WorldInfo info)
        {
            Tiles.Clear();
            Entities.Clear();
            CameraRig.ResetAngle();
        }

        private void OnEntityAdded(ClientEntity e) => Entities.Add(e, e.Id == _world.LocalEntityId);

        /// <summary>Whoever fires shows the attack frames towards the shot (the local player at its own attack rate).</summary>
        private void OnProjectileSpawned(ClientProjectile p)
        {
            var period = 0f;
            if (p.OwnerId == _world.LocalEntityId && _world.Stats != null && _world.Items.Count > 0)
            {
                var weapon = _world.Content.Item(_world.Items[0]);
                period = CombatRules.AttackPeriodMs(_world.Stats.Dexterity, weapon?.RateOfFire ?? 1f);
            }
            Entities.Attack(p.OwnerId, p.Angle, period);
        }
        private void OnEntityRemoved(ClientEntity e) => Entities.Remove(e.Id);

        /// <summary>World position (tiles, y south) under a screen point, for aiming.</summary>
        public Domain.World.Vec2 ScreenToWorld(Vector2 screen)
        {
            var cam = CameraRig.Camera;
            var p = cam.ScreenToWorldPoint(new Vector3(screen.x, screen.y, -cam.transform.position.z));
            return new Domain.World.Vec2(p.x, -p.y);
        }

        /// <summary>Call after the session's Update each frame.</summary>
        public void Render()
        {
            if (_world == null || !_world.InWorld) return;
            var me = _world.Predictor.RenderPosition();
            CameraRig.Follow(Coordinates.ToUnity(me.X, me.Y));
            var deltaMs = Time.unscaledDeltaTime * 1000f;
            var speed = _world.Stats != null ? MovementRules.MoveSpeed(_world.Stats.Speed, 1f) : 0f;
            var cam = new Vector2(me.X, me.Y);
            Entities.Refresh(-CameraRig.AngleDegrees, cam, deltaMs, speed);
            Tiles.Refresh(cam, -CameraRig.AngleDegrees);
            Texts.SetCameraAngle(-CameraRig.AngleDegrees);
        }

        private void OnDestroy() => Detach();
    }
}

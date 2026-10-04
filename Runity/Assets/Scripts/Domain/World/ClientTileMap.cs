using Runity.Domain.Content;

namespace Runity.Domain.World
{
    /// <summary>Tile flags; the same values and meaning as the server's sim::tile_flags.</summary>
    public static class TileFlags
    {
        public const byte NoWalk = 1 << 0;
        public const byte OccupySquare = 1 << 1;
        public const byte FullOccupy = 1 << 2;
        public const byte EnemyOccupySquare = 1 << 3;
        public const byte BlocksSight = 1 << 4;
        public const byte Void = 1 << 5;
    }

    public struct ClientTile
    {
        public ushort Ground;
        public ushort Object;   // static object type, 0 = none
        public byte Flags;
        public float Speed;
        public bool Sinking;
        public bool Known;      // received from the server
    }

    /// <summary>The client's copy of the world's tiles, filled from TileData. A tile not received yet counts as void, exactly like the
    /// server's out-of-map tile, so prediction never walks into the unknown.</summary>
    public sealed class ClientTileMap
    {
        private static readonly ClientTile VoidTile = new ClientTile { Ground = 0xFF, Flags = TileFlags.Void | TileFlags.NoWalk, Speed = 1f };
        private readonly ClientTile[] _tiles;

        public int Width { get; }
        public int Height { get; }

        public ClientTileMap(int width, int height)
        {
            Width = width;
            Height = height;
            _tiles = new ClientTile[width * height];
            for (var i = 0; i < _tiles.Length; i++) _tiles[i] = VoidTile;
        }

        public bool InBounds(int x, int y) => x >= 0 && y >= 0 && x < Width && y < Height;

        public ClientTile At(int x, int y) => InBounds(x, y) ? _tiles[y * Width + x] : VoidTile;

        /// <summary>Applies one received tile, deriving its flags from the definitions the same way the server's TileMap::build does.</summary>
        public void Set(int x, int y, ushort ground, ushort objectType, ContentCatalog content)
        {
            if (!InBounds(x, y)) return;
            var tile = new ClientTile { Ground = ground, Object = objectType, Speed = 1f, Known = true };
            var g = ground == 0xFF ? null : content.Ground(ground);
            if (g != null)
            {
                if (g.NoWalk) tile.Flags |= TileFlags.NoWalk;
                tile.Speed = g.Speed;
                tile.Sinking = g.Sinking;
            }
            else
            {
                tile.Flags |= TileFlags.Void | TileFlags.NoWalk;
            }
            if (objectType != 0)
            {
                var o = content.Object(objectType);
                if (o != null)
                {
                    if (o.OccupySquare) tile.Flags |= TileFlags.OccupySquare;
                    if (o.FullOccupy) tile.Flags |= TileFlags.FullOccupy;
                    if (o.EnemyOccupySquare) tile.Flags |= TileFlags.EnemyOccupySquare;
                    if (o.BlocksSight) tile.Flags |= TileFlags.BlocksSight;
                }
            }
            _tiles[y * Width + x] = tile;
        }

        /// <summary>Test helper: a tile with explicit values.</summary>
        public void SetRaw(int x, int y, ClientTile tile)
        {
            if (InBounds(x, y)) _tiles[y * Width + x] = tile;
        }
    }
}

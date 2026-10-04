using UnityEngine;

namespace Runity.Presentation.World
{
    /// <summary>World (server) coordinates are tiles with y pointing south; Unity's y points up. One tile = one Unity unit.</summary>
    public static class Coordinates
    {
        public static Vector3 ToUnity(float x, float y, float z = 0f) => new Vector3(x, -y, z);

        /// <summary>The Tilemap cell that covers world tile (x, y): it spans x..x+1 and -(y+1)..-y in Unity.</summary>
        public static Vector3Int TileCell(int x, int y) => new Vector3Int(x, -y - 1, 0);
    }
}

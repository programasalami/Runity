using System;
using System.IO;

namespace Runity.Client.Tests
{
    /// <summary>Finds the repository root (the folder holding Protocol/ and Content/) from Unity's project folder or a dotnet test bin.</summary>
    public static class TestPaths
    {
        public static string RepoRoot()
        {
            foreach (var start in new[] { Directory.GetCurrentDirectory(), AppContext.BaseDirectory })
            {
                var dir = new DirectoryInfo(start);
                while (dir != null)
                {
                    if (Directory.Exists(Path.Combine(dir.FullName, "Protocol", "vectors")) && Directory.Exists(Path.Combine(dir.FullName, "Content")))
                        return dir.FullName;
                    dir = dir.Parent;
                }
            }
            throw new DirectoryNotFoundException("repository root (Protocol/ + Content/) not found");
        }

        public static string Definitions => Path.Combine(RepoRoot(), "Content", "Definitions");
        public static string Vectors => Path.Combine(RepoRoot(), "Protocol", "vectors");
    }
}

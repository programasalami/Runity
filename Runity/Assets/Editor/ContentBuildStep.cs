using System.IO;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace Runity.EditorTools
{
    /// <summary>A player build reads the game definitions from StreamingAssets/Content/Definitions; in the Editor they are read from
    /// the repository's Content folder directly. This step copies them in before a build and removes the copy afterwards, so the
    /// repository keeps exactly one source of truth (Content/).</summary>
    public sealed class ContentBuildStep : IPreprocessBuildWithReport, IPostprocessBuildWithReport
    {
        private const string Target = "Assets/StreamingAssets/Content/Definitions";
        private const string ArtTarget = "Assets/StreamingAssets/Content/Art";
        public int callbackOrder => 0;

        private static string Source => Path.GetFullPath(Path.Combine(Application.dataPath, "..", "..", "Content", "Definitions"));

        public void OnPreprocessBuild(BuildReport report)
        {
            if (!Directory.Exists(Source)) throw new BuildFailedException($"game definitions not found at {Source}");
            Directory.CreateDirectory(Target);
            foreach (var file in Directory.GetFiles(Source, "*.xml")) File.Copy(file, Path.Combine(Target, Path.GetFileName(file)), true);
            // The art is read at runtime from plain files (no texture import), so it is copied as-is.
            var art = Path.Combine(Path.GetDirectoryName(Source), "Art");
            if (Directory.Exists(art))
            {
                FileUtil.DeleteFileOrDirectory(ArtTarget);
                FileUtil.CopyFileOrDirectory(art, ArtTarget);
            }
            AssetDatabase.Refresh();
        }

        public void OnPostprocessBuild(BuildReport report)
        {
            FileUtil.DeleteFileOrDirectory("Assets/StreamingAssets/Content");
            FileUtil.DeleteFileOrDirectory("Assets/StreamingAssets/Content.meta");
            AssetDatabase.Refresh();
        }
    }
}

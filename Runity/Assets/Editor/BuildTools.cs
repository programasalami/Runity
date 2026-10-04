using System.IO;
using UnityEditor;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace Runity.EditorTools
{
    /// <summary>Player builds. Batch mode: Unity -batchmode -projectPath Runity -executeMethod Runity.EditorTools.BuildTools.BuildWindows -quit
    /// Output: Builds/Windows/Runity.exe (next to the Runity project). The game definitions are copied in by ContentBuildStep.</summary>
    public static class BuildTools
    {
        /// <summary>Diagnostics: proves the editor starts and runs a method in batch mode.</summary>
        public static void Ping()
        {
            Debug.Log("[Runity] ping: the editor is up and running methods");
            EditorApplication.Exit(0);
        }

        [MenuItem("Runity/Build Windows Player")]
        public static void BuildWindows()
        {
            if (!BuildWindowsChecked() && Application.isBatchMode) EditorApplication.Exit(1);
        }

        /// <summary>Builds Builds/Windows/Runity.exe; false when the build fails.</summary>
        public static bool BuildWindowsChecked()
        {
            Debug.Log("[Runity] build: starting");
            if (!File.Exists(SceneBuilder.ScenePath)) SceneBuilder.Build();
            var output = Path.GetFullPath(Path.Combine(Application.dataPath, "..", "..", "Builds", "Windows", "Runity.exe"));
            var options = new BuildPlayerOptions
            {
                scenes = new[] { SceneBuilder.ScenePath },
                locationPathName = output,
                target = BuildTarget.StandaloneWindows64,
                options = BuildOptions.None,
            };
            PlayerSettings.productName = "Runity";
            PlayerSettings.fullScreenMode = FullScreenMode.Windowed;
            PlayerSettings.defaultScreenWidth = 1280;
            PlayerSettings.defaultScreenHeight = 720;
            PlayerSettings.runInBackground = true;  // keeps the connection alive while another window has focus
            Debug.Log("[Runity] build: calling BuildPipeline.BuildPlayer");
            var report = BuildPipeline.BuildPlayer(options);
            if (report.summary.result != BuildResult.Succeeded)
            {
                Debug.LogError($"[Runity] build failed: {report.summary.result}, {report.summary.totalErrors} errors");
                return false;
            }
            Debug.Log($"[Runity] built {output} ({report.summary.totalSize / (1024 * 1024)} MB)");
            return true;
        }
    }
}

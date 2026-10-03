using System;
using UnityEditor;
using UnityEngine;

namespace WaW.EditorTools
{
    /// <summary>Batch-mode driver for this machine, where the editor sometimes never reaches -executeMethod after loading.
    /// Set WAW_BATCH=ping|build|scene and run Unity -batchmode without -executeMethod: the job starts from the editor's own update
    /// loop and the editor quits when it ends. It also logs the editor state every 15 s, so a stall shows what the editor is waiting for.</summary>
    [InitializeOnLoad]
    public static class BatchDriver
    {
        private static readonly string Job = Environment.GetEnvironmentVariable("WAW_BATCH");
        private static double _nextReport;
        private static bool _started;

        static BatchDriver()
        {
            if (!Application.isBatchMode || string.IsNullOrEmpty(Job)) return;
            Debug.Log($"[WaW] batch: domain loaded, job '{Job}'");
            EditorApplication.update += Tick;
        }

        private static void Tick()
        {
            var now = EditorApplication.timeSinceStartup;
            if (now >= _nextReport)
            {
                _nextReport = now + 15;
                Debug.Log($"[WaW] batch: t={now:F0}s compiling={EditorApplication.isCompiling} updating={EditorApplication.isUpdating} started={_started}");
            }
            if (_started || EditorApplication.isCompiling || EditorApplication.isUpdating) return;
            _started = true;
            var code = 0;
            try
            {
                switch (Job)
                {
                    case "ping": Debug.Log("[WaW] ping: the editor is up and running methods"); break;
                    case "scene": SceneBuilder.Build(); break;
                    case "build": code = BuildTools.BuildWindowsChecked() ? 0 : 1; break;
                    default: Debug.LogError($"[WaW] batch: unknown job '{Job}'"); code = 2; break;
                }
            }
            catch (Exception e)
            {
                Debug.LogException(e);
                code = 1;
            }
            Debug.Log($"[WaW] batch: job '{Job}' finished, exit {code}");
            EditorApplication.Exit(code);
        }
    }
}

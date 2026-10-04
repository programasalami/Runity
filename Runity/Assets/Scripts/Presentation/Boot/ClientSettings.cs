using System;
using System.IO;
using UnityEngine;

namespace Runity.Presentation.Boot
{
    /// <summary>Where the client connects and how it starts. Defaults suit local development; the launcher and the command line
    /// override them (the reference launcher hands over RUNITY_LAUNCH_USER / RUNITY_LAUNCH_TOKEN).</summary>
    public sealed class ClientSettings
    {
        public string ApiUrl = "http://127.0.0.1:5080/";
        public string BuildVersion = "0.1.0";
        public string LaunchUser;
        public string LaunchToken;
        /// <summary>Development / test automation: log in (registering first if needed) and enter the world without UI input.</summary>
        public string AutoLoginName;
        public string AutoLoginPassword;

        public static ClientSettings FromEnvironment()
        {
            var s = new ClientSettings
            {
                ApiUrl = Environment.GetEnvironmentVariable("RUNITY_API_URL") ?? "http://127.0.0.1:5080/",
                LaunchUser = Environment.GetEnvironmentVariable("RUNITY_LAUNCH_USER"),
                LaunchToken = Environment.GetEnvironmentVariable("RUNITY_LAUNCH_TOKEN"),
                AutoLoginName = Environment.GetEnvironmentVariable("RUNITY_AUTOLOGIN_NAME"),
                AutoLoginPassword = Environment.GetEnvironmentVariable("RUNITY_AUTOLOGIN_PASSWORD"),
            };
            var args = Environment.GetCommandLineArgs();
            for (var i = 0; i < args.Length - 1; i++)
            {
                if (args[i] == "-runity-api") s.ApiUrl = args[i + 1];
            }
            return s;
        }

        /// <summary>The art (Game.atlas + Sheets): Content/Art beside the definitions, wherever those are.</summary>
        public static string ArtDirectory() => Path.Combine(Path.GetDirectoryName(DefinitionsDirectory()), "Art");

        /// <summary>The gameplay definitions: the repository's Content folder in the Editor, StreamingAssets in a build
        /// (copied there by the build step ContentBuildStep).</summary>
        public static string DefinitionsDirectory()
        {
#if UNITY_EDITOR
            return Path.GetFullPath(Path.Combine(Application.dataPath, "..", "..", "Content", "Definitions"));
#else
            return Path.Combine(Application.streamingAssetsPath, "Content", "Definitions");
#endif
        }
    }
}

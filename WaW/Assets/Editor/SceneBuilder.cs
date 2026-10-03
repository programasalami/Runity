using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using WaW.Presentation.Art;
using WaW.Presentation.Boot;

namespace WaW.EditorTools
{
    /// <summary>Builds the client's scene and its few assets from code, so nothing depends on hand-edited scene files:
    ///   menu WaW > Build Game Scene, or batch mode: Unity -batchmode -projectPath WaW -executeMethod WaW.EditorTools.SceneBuilder.Build -quit</summary>
    public static class SceneBuilder
    {
        public const string ScenePath = "Assets/Scenes/Game.unity";
        public const string MaterialPath = "Assets/Presentation/Materials/SpriteUnlit.mat";
        public const string ArtCatalogPath = "Assets/Presentation/ArtCatalog.asset";

        [MenuItem("WaW/Build Game Scene")]
        public static void Build()
        {
            Directory.CreateDirectory("Assets/Presentation/Materials");
            Directory.CreateDirectory("Assets/Scenes");

            var material = AssetDatabase.LoadAssetAtPath<Material>(MaterialPath);
            if (material == null)
            {
                var shader = Shader.Find("Universal Render Pipeline/2D/Sprite-Unlit-Default");
                if (shader == null) shader = Shader.Find("Sprites/Default");
                material = new Material(shader) { name = "SpriteUnlit" };
                AssetDatabase.CreateAsset(material, MaterialPath);
            }

            var art = AssetDatabase.LoadAssetAtPath<ArtCatalog>(ArtCatalogPath);
            if (art == null)
            {
                art = ScriptableObject.CreateInstance<ArtCatalog>();
                AssetDatabase.CreateAsset(art, ArtCatalogPath);
            }

            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
            var camera = new GameObject("Main Camera", typeof(Camera), typeof(AudioListener));
            camera.tag = "MainCamera";
            var cam = camera.GetComponent<Camera>();
            cam.orthographic = true;
            cam.orthographicSize = 8;
            cam.transform.position = new Vector3(0, 0, -10);

            var game = new GameObject("Game");
            var bootstrap = game.AddComponent<GameBootstrap>();
            bootstrap.SpriteMaterial = material;
            bootstrap.Art = art;

            EditorSceneManager.SaveScene(scene, ScenePath);
            EditorBuildSettings.scenes = new[] { new EditorBuildSettingsScene(ScenePath, true) };
            AssetDatabase.SaveAssets();
            Debug.Log($"[WaW] built {ScenePath}");
        }
    }
}

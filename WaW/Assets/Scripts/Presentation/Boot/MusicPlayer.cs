using System.Collections;
using System.IO;
using UnityEngine;
using UnityEngine.Networking;

namespace WaW.Presentation.Boot
{
    /// <summary>The original's music channel: Content/Art/Sound/Music/sorc.ogg, looping, faded in over 2 s at start
    /// (Audio.MusicChannel.FadeTo). The on/off choice is remembered (Settings.PlayMusic).</summary>
    public sealed class MusicPlayer : MonoBehaviour
    {
        private const string PrefKey = "waw.playMusic";
        private const float Volume = 0.6f;
        private AudioSource _source;
        private float _target;

        public static bool Enabled
        {
            get => PlayerPrefs.GetInt(PrefKey, 1) != 0;
            private set => PlayerPrefs.SetInt(PrefKey, value ? 1 : 0);
        }

        public void Play(string relativePath)
        {
            _source = gameObject.AddComponent<AudioSource>();
            _source.loop = true;
            _source.volume = 0f;
            _target = Enabled ? Volume : 0f;
            StartCoroutine(Load(Path.Combine(ClientSettings.ArtDirectory(), relativePath)));
        }

        public void Toggle()
        {
            Enabled = !Enabled;
            _target = Enabled ? Volume : 0f;
        }

        private IEnumerator Load(string path)
        {
            if (!File.Exists(path)) yield break;
            using var request = UnityWebRequestMultimedia.GetAudioClip("file:///" + path.Replace('\\', '/'), AudioType.OGGVORBIS);
            yield return request.SendWebRequest();
            if (request.result != UnityWebRequest.Result.Success)
            {
                Debug.LogWarning("[WaW] music: " + request.error);
                yield break;
            }
            _source.clip = DownloadHandlerAudioClip.GetContent(request);
            _source.Play();
        }

        private void Update()
        {
            if (_source == null) return;
            // 2 s fade towards the target volume (on / off).
            _source.volume = Mathf.MoveTowards(_source.volume, _target, Volume / 2f * Time.unscaledDeltaTime);
        }
    }
}

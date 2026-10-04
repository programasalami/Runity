using System;
using System.Collections.Generic;
using System.IO;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.UI;
using Runity.Presentation.Art;
using Runity.Presentation.Boot;

namespace Runity.Presentation.UI.Alloy
{
    public enum FontType
    {
        Normal,
        Bold,
        Bolder,
    }

    /// <summary>The original client's UI conventions (AlloyClient Alloy.UiLib), rebuilt on uGUI: a 1280x720 top-left pixel layout, Myriad
    /// Pro text with a soft black glow, text buttons that turn #FFDC85 on hover, fades through black, overlays and dialogs.
    /// Coordinates are the original's: x right, y DOWN from the top-left of the 1280x720 reference screen.</summary>
    public static class AlloyUi
    {
        public static readonly Color White = Hex(0xFFFFFF);
        public static readonly Color Hover = Hex(0xFFDC85);
        public static readonly Color Muted = Hex(0xB3B3B3);
        public static readonly Color Darken = Hex(0x2B2B2B, 0.8f);

        private static Font[] _fonts;
        private static readonly Dictionary<string, Sprite> UiSprites = new Dictionary<string, Sprite>();

        public static Color Hex(uint rgb, float alpha = 1f) =>
            new Color(((rgb >> 16) & 0xFF) / 255f, ((rgb >> 8) & 0xFF) / 255f, (rgb & 0xFF) / 255f, alpha);

        public static Font Font(FontType type)
        {
            if (_fonts == null)
            {
                var fallback = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
                _fonts = new[]
                {
                    Resources.Load<Font>("Fonts/MyriadPro") ?? fallback,
                    Resources.Load<Font>("Fonts/MyriadProBold") ?? fallback,
                    Resources.Load<Font>("Fonts/MyriadProBolder") ?? fallback,
                };
            }
            return _fonts[(int)type];
        }

        // ---- layout ---------------------------------------------------------------------------------------------------------

        public static RectTransform Node(Transform parent, string name)
        {
            var go = new GameObject(name, typeof(RectTransform));
            go.transform.SetParent(parent, false);
            return (RectTransform)go.transform;
        }

        /// <summary>Places a rect like the original: (x, y) is where the element's anchor point sits, measured from the parent's top-left
        /// with y down; pivot uses Unity's convention (0,1 = LeftTop, 0.5,0.5 = Middle, 1,0 = RightBottom).</summary>
        public static RectTransform At(this RectTransform r, float x, float y, float w, float h, Vector2 pivot)
        {
            r.anchorMin = r.anchorMax = new Vector2(0, 1);
            r.pivot = pivot;
            r.sizeDelta = new Vector2(w, h);
            r.anchoredPosition = new Vector2(x, -y);
            return r;
        }

        public static RectTransform Fill(this RectTransform r)
        {
            r.anchorMin = Vector2.zero;
            r.anchorMax = Vector2.one;
            r.offsetMin = r.offsetMax = Vector2.zero;
            return r;
        }

        public static readonly Vector2 LeftTop = new Vector2(0, 1);
        public static readonly Vector2 MiddleTop = new Vector2(0.5f, 1);
        public static readonly Vector2 RightTop = new Vector2(1, 1);
        public static readonly Vector2 MiddleLeft = new Vector2(0, 0.5f);
        public static readonly Vector2 Middle = new Vector2(0.5f, 0.5f);
        public static readonly Vector2 MiddleRight = new Vector2(1, 0.5f);
        public static readonly Vector2 LeftBottom = new Vector2(0, 0);
        public static readonly Vector2 MiddleBottom = new Vector2(0.5f, 0);
        public static readonly Vector2 RightBottom = new Vector2(1, 0);

        public static Image Rect(Transform parent, string name, Color color)
        {
            var r = Node(parent, name);
            var img = r.gameObject.AddComponent<Image>();
            img.color = color;
            return img;
        }

        // ---- text -----------------------------------------------------------------------------------------------------------

        /// <summary>A text sized to its content (like the original's glyph-box size). glow = OutlineThickness: the original's soft black
        /// halo, approximated with a faint outline.</summary>
        public static Text Text(Transform parent, string text, int size, FontType font = FontType.Normal, Color? color = null,
            float glow = 0, float maxWidth = -1)
        {
            var r = Node(parent, "Text");
            var t = r.gameObject.AddComponent<Text>();
            t.font = Font(font);
            t.fontSize = size;
            t.color = color ?? White;
            t.text = text;
            t.supportRichText = false;
            t.alignment = TextAnchor.MiddleCenter;
            t.horizontalOverflow = maxWidth > 0 ? HorizontalWrapMode.Wrap : HorizontalWrapMode.Overflow;
            t.verticalOverflow = VerticalWrapMode.Overflow;
            t.raycastTarget = false;
            var fit = r.gameObject.AddComponent<ContentSizeFitter>();
            fit.horizontalFit = maxWidth > 0 ? ContentSizeFitter.FitMode.Unconstrained : ContentSizeFitter.FitMode.PreferredSize;
            fit.verticalFit = ContentSizeFitter.FitMode.PreferredSize;
            if (maxWidth > 0)
            {
                r.sizeDelta = new Vector2(maxWidth, size);
                t.alignment = TextAnchor.UpperLeft;
            }
            if (glow > 0)
            {
                var o = r.gameObject.AddComponent<Outline>();
                o.effectColor = new Color(0, 0, 0, Mathf.Clamp01(0.0625f * glow + 0.1f));
                o.effectDistance = new Vector2(1.2f, -1.2f);
            }
            return t;
        }

        public static Text Place(this Text t, float x, float y, Vector2 pivot)
        {
            var r = t.rectTransform;
            r.anchorMin = r.anchorMax = new Vector2(0, 1);
            r.pivot = pivot;
            r.anchoredPosition = new Vector2(x, -y);
            return t;
        }

        /// <summary>The original TextButton: Bold white text, #FFDC85 on hover, click on release over the same button.</summary>
        public static Text TextButton(Transform parent, string text, int size, Action onClick, FontType font = FontType.Bold,
            float glow = 0, Color? color = null)
        {
            var t = Text(parent, text, size, font, color, glow);
            t.raycastTarget = true;
            var h = t.gameObject.AddComponent<HoverText>();
            h.Init(t, color ?? White, onClick);
            return t;
        }

        /// <summary>The title / menu bar buttons: Bold, glow 4, optional "pulse" (scale 1.00..1.10, period ~1.26 s).</summary>
        public static Text MenuButton(Transform parent, string text, int size, Action onClick, bool pulse = false)
        {
            var t = TextButton(parent, text, size, onClick, FontType.Bold, 4);
            if (pulse) t.gameObject.AddComponent<Pulse>();
            return t;
        }

        // ---- art ------------------------------------------------------------------------------------------------------------

        /// <summary>A UI picture from Content/Art (e.g. "Ui/TextBox.png", "TitleScreen/TitleScreenGraphic.png"), with an optional
        /// 9-slice border in source pixels.</summary>
        public static Sprite UiSprite(string relativePath, int border = 0)
        {
            var key = relativePath + "#" + border;
            if (UiSprites.TryGetValue(key, out var s)) return s;
            var path = Path.Combine(ClientSettings.ArtDirectory(), relativePath);
            if (File.Exists(path))
            {
                var tex = new Texture2D(2, 2, TextureFormat.RGBA32, false) { filterMode = FilterMode.Point, wrapMode = TextureWrapMode.Clamp };
                if (tex.LoadImage(File.ReadAllBytes(path), false))
                {
                    s = Sprite.Create(tex, new Rect(0, 0, tex.width, tex.height), new Vector2(0.5f, 0.5f), 100, 0, SpriteMeshType.FullRect,
                        new Vector4(border, border, border, border));
                }
            }
            if (s == null) Debug.LogWarning($"[Runity] ui art missing: {relativePath}");
            UiSprites[key] = s;
            return s;
        }

        public static Image Picture(Transform parent, Sprite sprite, string name = "Picture")
        {
            var r = Node(parent, name);
            var img = r.gameObject.AddComponent<Image>();
            img.sprite = sprite;
            img.raycastTarget = false;
            img.enabled = sprite != null;
            return img;
        }

        /// <summary>A game-atlas sprite drawn in the UI with the original's black outline (ObjectRect with outline/glow).</summary>
        public static Image GameSprite(Transform parent, Sprite sprite, bool outline = true, string name = "Sprite")
        {
            var img = Picture(parent, sprite, name);
            img.preserveAspect = true;
            if (outline && sprite != null)
            {
                var o = img.gameObject.AddComponent<Outline>();
                o.effectColor = new Color(0, 0, 0, 0.9f);
                o.effectDistance = new Vector2(1.5f, -1.5f);
                var s = img.gameObject.AddComponent<Shadow>();
                s.effectColor = new Color(0, 0, 0, 0.45f);
                s.effectDistance = new Vector2(0, -2.5f);
            }
            return img;
        }

        public static Sprite Players(int index, Facing facing = Facing.Down, int frame = 0) =>
            SheetAtlas.Shared?.Frame("players", index, facing, frame);

        // ---- tweens ---------------------------------------------------------------------------------------------------------

        public static float SineInOut(float r) => -0.5f * (Mathf.Cos(Mathf.PI * r) - 1f);

        public static CanvasGroup Group(GameObject go) => go.TryGetComponent(out CanvasGroup g) ? g : go.AddComponent<CanvasGroup>();

        /// <summary>Fades a group's alpha (SineInOut) over ms milliseconds, then runs done. Interaction is off while fading, as in the
        /// original (no input while an alpha tween runs).</summary>
        public static void Fade(GameObject go, float from, float to, float ms, Action done = null, float delayMs = 0)
        {
            var g = Group(go);
            if (!go.TryGetComponent(out AlphaTween tween)) tween = go.AddComponent<AlphaTween>();
            tween.Begin(g, from, to, ms / 1000f, delayMs / 1000f, done);
        }
    }

    public sealed class HoverText : MonoBehaviour, IPointerEnterHandler, IPointerExitHandler, IPointerDownHandler, IPointerUpHandler
    {
        private Text _text;
        private Color _active;
        private Action _onClick;
        private bool _down;
        private bool _over;

        public Color HoverColor = AlloyUi.Hover;

        public void Init(Text text, Color active, Action onClick)
        {
            _text = text;
            _active = active;
            _onClick = onClick;
        }

        public void SetActiveColor(Color c)
        {
            _active = c;
            if (!_over) _text.color = c;
        }

        public void OnPointerEnter(PointerEventData e)
        {
            _over = true;
            _text.color = HoverColor;
        }

        public void OnPointerExit(PointerEventData e)
        {
            _over = false;
            _down = false;
            _text.color = _active;
        }

        public void OnPointerDown(PointerEventData e) => _down = e.button == PointerEventData.InputButton.Left;

        public void OnPointerUp(PointerEventData e)
        {
            if (_down && _over && e.button == PointerEventData.InputButton.Left) _onClick?.Invoke();
            _down = false;
        }

        private void OnDisable()
        {
            _over = _down = false;
            if (_text != null) _text.color = _active;
        }
    }

    /// <summary>Clicks on a whole rect (server rects, cards) with a hover colour on its background.</summary>
    public sealed class HoverRect : MonoBehaviour, IPointerEnterHandler, IPointerExitHandler, IPointerClickHandler
    {
        public Image Background;
        public Color Normal;
        public Color Hovered;
        public Action Clicked;

        public void OnPointerEnter(PointerEventData e)
        {
            if (Background != null) Background.color = Hovered;
        }

        public void OnPointerExit(PointerEventData e)
        {
            if (Background != null) Background.color = Normal;
        }

        public void OnPointerClick(PointerEventData e)
        {
            if (e.button == PointerEventData.InputButton.Left) Clicked?.Invoke();
        }
    }

    public sealed class Pulse : MonoBehaviour
    {
        private void Update()
        {
            var s = 1.05f + 0.05f * Mathf.Sin(Time.unscaledTime * 1000f / 200f);
            transform.localScale = new Vector3(s, s, 1);
        }
    }

    public sealed class AlphaTween : MonoBehaviour
    {
        private CanvasGroup _group;
        private float _from;
        private float _to;
        private float _duration;
        private float _delay;
        private float _t;
        private Action _done;
        private bool _running;

        public void Begin(CanvasGroup g, float from, float to, float seconds, float delay, Action done)
        {
            _group = g;
            _from = from;
            _to = to;
            _duration = Mathf.Max(0.0001f, seconds);
            _delay = delay;
            _t = 0;
            _done = done;
            _running = true;
            g.alpha = from;
            g.interactable = g.blocksRaycasts = false;
            enabled = true;
        }

        private void Update()
        {
            if (!_running) return;
            if (_delay > 0)
            {
                _delay -= Time.unscaledDeltaTime;
                return;
            }
            _t += Time.unscaledDeltaTime;
            var r = Mathf.Clamp01(_t / _duration);
            _group.alpha = Mathf.Lerp(_from, _to, AlloyUi.SineInOut(r));
            if (r < 1f) return;
            _running = false;
            _group.interactable = _group.blocksRaycasts = _to > 0f;
            var done = _done;
            _done = null;
            done?.Invoke();
        }
    }
}

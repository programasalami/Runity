using System;
using UnityEngine;
using UnityEngine.Events;
using UnityEngine.EventSystems;
using UnityEngine.InputSystem.UI;
using UnityEngine.UI;

namespace Runity.Presentation.UI
{
    /// <summary>Small builder for the uGUI screens, made in code (no prefabs to keep in sync). The look is deliberately plain: the
    /// game's real UI art is sourced separately and replaces these panels' sprites and fonts without changing the screen logic.</summary>
    public sealed class Ui
    {
        public static readonly Color PanelColor = new Color(0.08f, 0.08f, 0.11f, 0.92f);
        public static readonly Color ButtonColor = new Color(0.22f, 0.24f, 0.32f, 1f);
        public static readonly Color FieldColor = new Color(0.14f, 0.14f, 0.18f, 1f);
        public static readonly Color TextColor = new Color(0.92f, 0.92f, 0.95f, 1f);
        public static readonly Color DimText = new Color(0.65f, 0.67f, 0.75f, 1f);

        public Font Font { get; }
        public Canvas Canvas { get; }

        public Ui(Font font)
        {
            Font = font;
            var canvasGo = new GameObject("UI", typeof(Canvas), typeof(CanvasScaler), typeof(GraphicRaycaster));
            Canvas = canvasGo.GetComponent<Canvas>();
            Canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            var scaler = canvasGo.GetComponent<CanvasScaler>();
            scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
            scaler.referenceResolution = new Vector2(1280, 720);
            scaler.screenMatchMode = CanvasScaler.ScreenMatchMode.Expand;  // the original: uniform min(W/1280, H/720)
            if (UnityEngine.Object.FindAnyObjectByType<EventSystem>() == null)
                new GameObject("EventSystem", typeof(EventSystem), typeof(InputSystemUIInputModule));
        }

        public static RectTransform Rect(GameObject go) => go.TryGetComponent(out RectTransform r) ? r : go.AddComponent<RectTransform>();

        public static void Place(RectTransform r, Vector2 anchorMin, Vector2 anchorMax, Vector2 offsetMin, Vector2 offsetMax)
        {
            r.anchorMin = anchorMin;
            r.anchorMax = anchorMax;
            r.offsetMin = offsetMin;
            r.offsetMax = offsetMax;
        }

        public GameObject Panel(Transform parent, string name, Color color)
        {
            var go = new GameObject(name, typeof(RectTransform), typeof(Image));
            go.transform.SetParent(parent, false);
            go.GetComponent<Image>().color = color;
            return go;
        }

        /// <summary>A centred panel of a fixed size (the reference's rule: fixed sizes, never sized from children).</summary>
        public GameObject CenteredPanel(string name, float width, float height)
        {
            var go = Panel(Canvas.transform, name, PanelColor);
            var r = Rect(go);
            r.anchorMin = r.anchorMax = new Vector2(0.5f, 0.5f);
            r.sizeDelta = new Vector2(width, height);
            r.anchoredPosition = Vector2.zero;
            var layout = go.AddComponent<VerticalLayoutGroup>();
            layout.padding = new RectOffset(24, 24, 20, 20);
            layout.spacing = 10;
            layout.childControlHeight = true;
            layout.childControlWidth = true;
            layout.childForceExpandHeight = false;
            return go;
        }

        public Text Label(Transform parent, string text, int size = 20, TextAnchor anchor = TextAnchor.MiddleLeft, Color? color = null,
            float height = 30)
        {
            var go = new GameObject("Text", typeof(RectTransform), typeof(Text), typeof(LayoutElement));
            go.transform.SetParent(parent, false);
            var t = go.GetComponent<Text>();
            t.font = Font;
            t.fontSize = size;
            t.alignment = anchor;
            t.color = color ?? TextColor;
            t.text = text;
            t.horizontalOverflow = HorizontalWrapMode.Wrap;
            t.verticalOverflow = VerticalWrapMode.Truncate;
            go.GetComponent<LayoutElement>().preferredHeight = height;
            return t;
        }

        public InputField Field(Transform parent, string placeholder, bool password = false, float height = 38)
        {
            var go = new GameObject("Field", typeof(RectTransform), typeof(Image), typeof(InputField), typeof(LayoutElement));
            go.transform.SetParent(parent, false);
            go.GetComponent<Image>().color = FieldColor;
            go.GetComponent<LayoutElement>().preferredHeight = height;
            var field = go.GetComponent<InputField>();

            var text = Label(go.transform, "", 20);
            text.supportRichText = false;
            Place(text.rectTransform, Vector2.zero, Vector2.one, new Vector2(10, 4), new Vector2(-10, -4));
            var hint = Label(go.transform, placeholder, 20, TextAnchor.MiddleLeft, DimText);
            hint.fontStyle = FontStyle.Italic;
            Place(hint.rectTransform, Vector2.zero, Vector2.one, new Vector2(10, 4), new Vector2(-10, -4));

            field.textComponent = text;
            field.placeholder = hint;
            field.contentType = password ? InputField.ContentType.Password : InputField.ContentType.Standard;
            field.lineType = InputField.LineType.SingleLine;
            return field;
        }

        public Button Button(Transform parent, string label, UnityAction onClick, float height = 40)
        {
            var go = new GameObject("Button " + label, typeof(RectTransform), typeof(Image), typeof(Button), typeof(LayoutElement));
            go.transform.SetParent(parent, false);
            go.GetComponent<Image>().color = ButtonColor;
            go.GetComponent<LayoutElement>().preferredHeight = height;
            var b = go.GetComponent<Button>();
            b.onClick.AddListener(onClick);
            var t = Label(go.transform, label, 20, TextAnchor.MiddleCenter);
            Place(t.rectTransform, Vector2.zero, Vector2.one, Vector2.zero, Vector2.zero);
            return b;
        }

        /// <summary>A "&lt; value &gt;" picker cycling through a list.</summary>
        public Text Cycler(Transform parent, Func<int, string> labelOf, int count, Action<int> onChange)
        {
            var row = new GameObject("Cycler", typeof(RectTransform), typeof(HorizontalLayoutGroup), typeof(LayoutElement));
            row.transform.SetParent(parent, false);
            row.GetComponent<LayoutElement>().preferredHeight = 40;
            var h = row.GetComponent<HorizontalLayoutGroup>();
            h.spacing = 8;
            h.childControlWidth = true;
            h.childControlHeight = true;
            h.childForceExpandWidth = false;
            var index = 0;
            Text value = null;
            Button(row.transform, "<", () => { index = (index + count - 1) % count; value.text = labelOf(index); onChange(index); })
                .GetComponent<LayoutElement>().preferredWidth = 44;
            value = Label(row.transform, labelOf(0), 20, TextAnchor.MiddleCenter);
            value.GetComponent<LayoutElement>().flexibleWidth = 1;
            Button(row.transform, ">", () => { index = (index + 1) % count; value.text = labelOf(index); onChange(index); })
                .GetComponent<LayoutElement>().preferredWidth = 44;
            return value;
        }

        public static void Clear(Transform t)
        {
            for (var i = t.childCount - 1; i >= 0; i--) UnityEngine.Object.Destroy(t.GetChild(i).gameObject);
        }
    }
}

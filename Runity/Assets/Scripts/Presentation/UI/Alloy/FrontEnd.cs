using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using UnityEngine;
using UnityEngine.UI;
using Runity.Client;
using Runity.Domain.Content;
using Runity.Presentation.Art;

namespace Runity.Presentation.UI.Alloy
{
    /// <summary>One game server as the servers screen lists it.</summary>
    public sealed class ServerEntry
    {
        public string Name;
        public int Port;
        public int Players;
        public int MaxPlayers;
    }

    /// <summary>The original client's front end (AlloyClient Screens/*): loading, title, servers, legends, character list, the
    /// login / register overlays and the class selection wheel, at the original's 1280x720 layout. It shows data and raises
    /// requests; GameBootstrap does the HTTP and the game connection.</summary>
    public sealed class FrontEnd
    {
        public const float W = 1280;
        public const float H = 720;

        // Requests to the bootstrap. Login / Register return null on success, else the message for the error dialog.
        public Func<string, string, Task<string>> Login;
        public Func<string, string, Task<string>> Register;
        public Action Logout;
        public Action<int> PlayCharacter;
        public Action<ushort> CreateCharacter;
        public Action Quit;
        public Action ToggleMusic;
        public Func<bool> MusicOn;

        private readonly ContentCatalog _content;
        private readonly RectTransform _root;
        private readonly RectTransform _screenLayer;
        private readonly RectTransform _overlayLayer;
        private readonly RectTransform _dialogLayer;
        private readonly Image _fade;
        private readonly Queue<Action> _dialogs = new Queue<Action>();
        private GameObject _screen;
        private GameObject _overlayDim;
        private GameObject _overlay;
        private bool _dialogOpen;
        private Action _refreshAccountOverlay;

        public AccountInfo Account { get; private set; }
        public List<ServerEntry> Servers { get; } = new List<ServerEntry>();
        public int SelectedServerPort { get; private set; } = 2050;
        public string FailureMessage { get; set; }

        public FrontEnd(Canvas canvas, ContentCatalog content)
        {
            _content = content;
            _root = AlloyUi.Node(canvas.transform, "FrontEnd").Fill();
            _screenLayer = AlloyUi.Node(_root, "Screens").Fill();
            _fade = AlloyUi.Rect(_root, "Fade", Color.black);
            _fade.rectTransform.Fill();
            _fade.gameObject.SetActive(false);
            _overlayLayer = AlloyUi.Node(_root, "Overlays").Fill();
            _dialogLayer = AlloyUi.Node(_root, "Dialogs").Fill();
        }

        public bool Visible => _root.gameObject.activeSelf;

        public void Show(bool visible)
        {
            _root.gameObject.SetActive(visible);
            if (!visible) CloseOverlay(instant: true);
        }

        public void SetAccount(AccountInfo account)
        {
            Account = account;
            _refreshAccountOverlay?.Invoke();
        }

        // ---- navigation -----------------------------------------------------------------------------------------------------

        /// <summary>ScreenManager.FadeToScreen: the old screen fades to black in d/2, the new one fades in over the next d/2.</summary>
        public void FadeTo(Func<GameObject> build, float ms = 1000)
        {
            _fade.gameObject.SetActive(true);
            _fade.transform.SetSiblingIndex(0);
            var old = _screen;
            void Swap()
            {
                if (old != null) UnityEngine.Object.Destroy(old);
                _screen = build();
                AlloyUi.Fade(_screen, 0, 1, ms / 2, () => _fade.gameObject.SetActive(false));
            }
            if (old != null) AlloyUi.Fade(old, AlloyUi.Group(old).alpha, 0, ms / 2, Swap);
            else Swap();
        }

        public void ShowLoading() => FadeTo(BuildLoading);
        public void ShowTitle(float ms = 1000) => FadeTo(BuildTitle, ms);
        public void ShowCharacters(float ms = 1000) => FadeTo(BuildCharacterList, ms);

        // ---- screen base (TitleScreenBase) ----------------------------------------------------------------------------------

        private enum ScreenKind
        {
            Loading,
            Title,
            Other,
        }

        private RectTransform ScreenBase(string name, ScreenKind kind)
        {
            var r = AlloyUi.Node(_screenLayer, name).Fill();
            // Background: cover-fit 1280x720 picture.
            var bg = AlloyUi.Picture(r, AlloyUi.UiSprite(kind == ScreenKind.Title ? "TitleScreen/TitleScreenGraphic.png" : "TitleScreen/TitleScreenBackground.png"), "Background");
            bg.rectTransform.Fill();
            var fit = bg.gameObject.AddComponent<AspectRatioFitter>();
            fit.aspectMode = AspectRatioFitter.AspectMode.EnvelopeParent;
            fit.aspectRatio = W / H;
            if (kind == ScreenKind.Other) AlloyUi.Rect(r, "Darken", AlloyUi.Darken).rectTransform.Fill();
            MusicButton(r);
            if (kind != ScreenKind.Loading) AccountOverlay(r);
            return r;
        }

        /// <summary>The music on/off icon at (7, 7): lofiInterfaceBig 3 (on) / 4 (off), #FFDC85 on hover.</summary>
        private void MusicButton(RectTransform screen)
        {
            Sprite Icon() => SheetAtlas.Shared?.Static("lofiInterfaceBig", MusicOn != null && MusicOn() ? 3 : 4);
            var img = AlloyUi.GameSprite(screen, Icon(), false, "Music");
            img.rectTransform.At(7, 7, 32, 32, AlloyUi.LeftTop);
            img.raycastTarget = true;
            var h = img.gameObject.AddComponent<HoverRect>();
            h.Background = img;
            h.Normal = Color.white;
            h.Hovered = AlloyUi.Hover;
            h.Clicked = () =>
            {
                ToggleMusic?.Invoke();
                img.sprite = Icon();
            };
        }

        /// <summary>Top-right "new account - register - login" / "logged in as X - logout".</summary>
        private void AccountOverlay(RectTransform screen)
        {
            var row = AlloyUi.Node(screen, "AccountOverlay").At(W - 10, 10, 10, 30, AlloyUi.RightTop);
            var layout = row.gameObject.AddComponent<HorizontalLayoutGroup>();
            layout.childAlignment = TextAnchor.UpperRight;
            layout.childControlWidth = layout.childControlHeight = false;
            layout.childForceExpandWidth = layout.childForceExpandHeight = false;
            row.gameObject.AddComponent<ContentSizeFitter>().horizontalFit = ContentSizeFitter.FitMode.PreferredSize;
            void Build()
            {
                if (row == null) return;
                for (var i = row.childCount - 1; i >= 0; i--) UnityEngine.Object.Destroy(row.GetChild(i).gameObject);
                if (Account == null)
                {
                    AlloyUi.Text(row, "new account - ", 24, FontType.Normal, AlloyUi.Muted);
                    AlloyUi.TextButton(row, "register", 24, () => SetOverlay(BuildRegister));
                    AlloyUi.Text(row, " - ", 24, FontType.Normal, AlloyUi.Muted);
                    AlloyUi.TextButton(row, "login", 24, () => SetOverlay(BuildLogin));
                }
                else
                {
                    AlloyUi.Text(row, $"logged in as {Account.Name} - ", 24);
                    AlloyUi.TextButton(row, "logout", 24, () =>
                    {
                        Logout?.Invoke();
                        Account = null;
                        ShowTitle(500);
                    });
                }
            }
            _refreshAccountOverlay = () =>
            {
                if (row == null) return;
                AlloyUi.Fade(row.gameObject, 1, 0, 150, () =>
                {
                    Build();
                    AlloyUi.Fade(row.gameObject, 0, 1, 150);
                });
            };
            Build();
        }

        private static Text MenuBar(RectTransform parent, string text, int size, Action click, bool pulse = false) =>
            AlloyUi.MenuButton(parent, text, size, click, pulse);

        // ---- loading / title --------------------------------------------------------------------------------------------------

        private GameObject BuildLoading()
        {
            var r = ScreenBase("LoadingScreen", ScreenKind.Loading);
            AlloyUi.Text(r, "Loading...", 40, FontType.Bold, null, 4).Place(W / 2, H - 90, AlloyUi.Middle);
            return r.gameObject;
        }

        private GameObject BuildTitle()
        {
            var r = ScreenBase("TitleScreen", ScreenKind.Title);
            // The menu bar: "play" centred at (640, 630), the others 50 px apart edge to edge.
            var bar = AlloyUi.Node(r, "MenuBar").At(W / 2, H - 90, 10, 60, AlloyUi.Middle);
            var layout = bar.gameObject.AddComponent<HorizontalLayoutGroup>();
            layout.spacing = 50;
            layout.childAlignment = TextAnchor.MiddleCenter;
            layout.childControlWidth = layout.childControlHeight = false;
            layout.childForceExpandWidth = layout.childForceExpandHeight = false;
            bar.gameObject.AddComponent<ContentSizeFitter>().horizontalFit = ContentSizeFitter.FitMode.PreferredSize;
            MenuBar(bar, "editor", 35, () => { });
            MenuBar(bar, "servers", 35, () => FadeTo(BuildServers));
            MenuBar(bar, "play", 57, () =>
            {
                if (Account != null) ShowCharacters();
                else SetOverlay(BuildLogin);
            }, pulse: true);
            MenuBar(bar, "legends", 35, () => FadeTo(BuildLegends));
            MenuBar(bar, "exit", 35, () => Quit?.Invoke());
            CheckForFailure(r);
            return r.gameObject;
        }

        private void CheckForFailure(RectTransform screen)
        {
            if (string.IsNullOrEmpty(FailureMessage)) return;
            var msg = FailureMessage;
            FailureMessage = null;
            var darken = AlloyUi.Rect(screen, "Darken", AlloyUi.Darken);
            darken.rectTransform.Fill();
            ShowDialog(msg, "", "Retry", () => ShowLoading(), "Quit", () => Quit?.Invoke());
        }

        // ---- overlays (OverlayManager) --------------------------------------------------------------------------------------

        public void SetOverlay(Func<RectTransform, GameObject> build)
        {
            if (_overlayDim == null)
            {
                var dim = AlloyUi.Rect(_overlayLayer, "OverlayDim", AlloyUi.Darken);
                dim.rectTransform.Fill();
                _overlayDim = dim.gameObject;
                AlloyUi.Fade(_overlayDim, 0, 1, 250);
                _overlay = build(_overlayLayer);
                AlloyUi.Fade(_overlay, 0, 1, 250);
                return;
            }
            var old = _overlay;
            if (old == null)
            {
                _overlay = build(_overlayLayer);
                AlloyUi.Fade(_overlay, 0, 1, 150);
                return;
            }
            AlloyUi.Fade(old, 1, 0, 150, () =>
            {
                UnityEngine.Object.Destroy(old);
                _overlay = build(_overlayLayer);
                AlloyUi.Fade(_overlay, 0, 1, 150);
            });
        }

        public void CloseOverlay(bool instant = false)
        {
            var dim = _overlayDim;
            var panel = _overlay;
            _overlayDim = null;
            _overlay = null;
            if (instant)
            {
                if (dim != null) UnityEngine.Object.Destroy(dim);
                if (panel != null) UnityEngine.Object.Destroy(panel);
                return;
            }
            if (dim != null) AlloyUi.Fade(dim, AlloyUi.Group(dim).alpha, 0, 250, () => UnityEngine.Object.Destroy(dim));
            if (panel != null) AlloyUi.Fade(panel, AlloyUi.Group(panel).alpha, 0, 175, () => UnityEngine.Object.Destroy(panel));
        }

        private GameObject BuildLogin(RectTransform layer) => AccountPanel(layer, "Log in", "Log in",
            "New user? Click here to Register!", () => SetOverlay(BuildRegister), async (u, p) =>
            {
                var error = Login != null ? await Login(u, p) : "Failed to contact server.";
                if (error != null)
                {
                    ShowDialog("Login Error", error, "Ok", null);
                    return;
                }
                CloseOverlay();
            });

        private GameObject BuildRegister(RectTransform layer) => AccountPanel(layer, "Register", "Create",
            "Existing user? Click here to login!", () => SetOverlay(BuildLogin), async (u, p) =>
            {
                var error = Register != null ? await Register(u, p) : "Failed to contact server.";
                if (error != null)
                {
                    ShowDialog("Register Error", error, "Ok", null);
                    return;
                }
                CloseOverlay();
                ShowTitle(500);
            });

        /// <summary>The 475x350 login / register panel centred on screen.</summary>
        private GameObject AccountPanel(RectTransform layer, string title, string confirm, string link, Action linkClick,
            Func<string, string, Task> submit)
        {
            var panel = AlloyUi.Rect(layer, title + "Panel", AlloyUi.Hex(0x363636));
            var r = panel.rectTransform;
            r.anchorMin = r.anchorMax = new Vector2(0.5f, 0.5f);
            r.pivot = new Vector2(0.5f, 0.5f);
            r.sizeDelta = new Vector2(475, 350);
            r.anchoredPosition = Vector2.zero;
            AlloyUi.Rect(r, "TitleBar", AlloyUi.Hex(0x4d4d4d)).rectTransform.At(0, 0, 475, 50, AlloyUi.LeftTop);
            AlloyUi.Text(r, title, 22, FontType.Bold).Place(237, 25, AlloyUi.Middle);
            var user = Input(r, "Username", 237, 100, false);
            var pass = Input(r, "Password", 237, 160, true);
            AlloyUi.TextButton(r, link, 16, linkClick).Place(237, 200, AlloyUi.Middle);
            var busy = false;
            async void Submit()
            {
                if (busy) return;
                busy = true;
                try
                {
                    await submit(user.text, pass.text);
                }
                finally
                {
                    busy = false;
                }
            }
            var ok = AlloyUi.TextButton(r, confirm, 28, Submit, FontType.Normal).Place(450, 325, AlloyUi.RightBottom);
            Canvas.ForceUpdateCanvases();
            AlloyUi.TextButton(r, "Cancel", 28, () => CloseOverlay(), FontType.Normal)
                .Place(450 - ok.preferredWidth - 35, 325, AlloyUi.RightBottom);
            // Unlike the original: typing starts in the username, Tab moves between the fields and Enter goes on / submits.
            var keys = panel.gameObject.AddComponent<FormKeys>();
            keys.Fields = new[] { user, pass };
            keys.Submit = Submit;
            keys.Blocked = () => _dialogOpen;
            user.Select();
            user.ActivateInputField();
            return panel.gameObject;
        }

        /// <summary>The original TextInput: white 2 px frame (Ui/TextBox.png 9-slice), Bold 24 white text, placeholder in the same
        /// white (shown only while empty and unfocused).</summary>
        private static InputField Input(RectTransform parent, string placeholder, float x, float y, bool password)
        {
            var box = AlloyUi.Picture(parent, AlloyUi.UiSprite("Ui/TextBox.png", 2), placeholder);
            box.type = Image.Type.Sliced;
            box.raycastTarget = true;
            box.rectTransform.At(x, y, 350, 34, AlloyUi.Middle);
            var field = box.gameObject.AddComponent<InputField>();
            var text = AlloyUi.Text(box.transform, "", 24, FontType.Bold, null, 4);
            UnityEngine.Object.Destroy(text.GetComponent<ContentSizeFitter>());
            text.alignment = TextAnchor.MiddleLeft;
            text.rectTransform.Fill();
            text.rectTransform.offsetMin = new Vector2(6, 0);
            text.rectTransform.offsetMax = new Vector2(-6, 0);
            var hint = AlloyUi.Text(box.transform, placeholder, 24, FontType.Bold, null, 4);
            UnityEngine.Object.Destroy(hint.GetComponent<ContentSizeFitter>());
            hint.alignment = TextAnchor.MiddleLeft;
            hint.rectTransform.Fill();
            hint.rectTransform.offsetMin = new Vector2(6, 0);
            field.textComponent = text;
            field.placeholder = hint;
            field.characterLimit = 255;
            field.contentType = password ? InputField.ContentType.Password : InputField.ContentType.Standard;
            field.lineType = InputField.LineType.SingleLine;
            field.caretColor = Color.white;
            field.customCaretColor = true;
            return field;
        }

        // ---- dialogs (DialogManager) ----------------------------------------------------------------------------------------

        /// <summary>Queued modal dialog: 300 wide, #1C1C1C @0.8; with a cancel option, confirm is on the RIGHT and cancel on the left.</summary>
        public void ShowDialog(string title, string message, string confirm, Action onConfirm, string cancel = null, Action onCancel = null)
        {
            _dialogs.Enqueue(() =>
            {
                _dialogOpen = true;
                var box = AlloyUi.Rect(_dialogLayer, "Dialog", AlloyUi.Hex(0x1C1C1C, 0.8f));
                var r = box.rectTransform;
                r.anchorMin = r.anchorMax = new Vector2(0.5f, 0.5f);
                r.pivot = new Vector2(0.5f, 0.5f);
                var t = AlloyUi.Text(r, title, 24, FontType.Bold).Place(150, 10, AlloyUi.MiddleTop);
                var m = AlloyUi.Text(r, message ?? "", 20).Place(150, 0, AlloyUi.MiddleTop);
                Canvas.ForceUpdateCanvases();
                var titleH = t.preferredHeight;
                var msgH = string.IsNullOrEmpty(message) ? 0 : m.preferredHeight;
                m.rectTransform.anchoredPosition = new Vector2(150, -(titleH + 20));
                const float buttonH = 26;
                var height = titleH + 10 + msgH + 20 + buttonH + 10;
                r.sizeDelta = new Vector2(300, height);
                var closing = false;
                void Close(Action then)
                {
                    if (closing) return;
                    closing = true;
                    AlloyUi.Fade(box.gameObject, 1, 0, 250, () =>
                    {
                        UnityEngine.Object.Destroy(box.gameObject);
                        _dialogOpen = false;
                        NextDialog();
                    });
                    then?.Invoke();
                }
                if (cancel == null)
                {
                    AlloyUi.TextButton(r, confirm, 22, () => Close(onConfirm)).Place(150, height - 10, AlloyUi.MiddleBottom);
                }
                else
                {
                    AlloyUi.TextButton(r, confirm, 22, () => Close(onConfirm)).Place(225, height - 10, AlloyUi.MiddleBottom);
                    AlloyUi.TextButton(r, cancel, 22, () => Close(onCancel)).Place(75, height - 10, AlloyUi.MiddleBottom);
                }
                box.gameObject.AddComponent<FormKeys>().Submit = () => Close(onConfirm);  // Enter confirms
                AlloyUi.Fade(box.gameObject, 0, 1, 250);
            });
            if (!_dialogOpen) NextDialog();
        }

        private void NextDialog()
        {
            if (_dialogOpen || _dialogs.Count == 0) return;
            _dialogs.Dequeue()();
        }

        // ---- servers / legends ----------------------------------------------------------------------------------------------

        public void SelectServer(int port) => SelectedServerPort = port;

        private GameObject BuildServers()
        {
            var r = ScreenBase("ServersScreen", ScreenKind.Other);
            AlloyUi.Text(r, "Server Selection", 32, FontType.Bold).Place(640, 50, AlloyUi.Middle);
            AlloyUi.Rect(r, "Divider", AlloyUi.Hex(0x404040)).rectTransform.At(0, 100, 1280, 5, AlloyUi.LeftTop);
            AlloyUi.Text(r, "Selected Server:", 22).Place(640, 115, AlloyUi.MiddleTop);
            var selected = Servers.FirstOrDefault(s => s.Port == SelectedServerPort) ?? Servers.FirstOrDefault();
            var selectedHolder = AlloyUi.Node(r, "Selected").At(640, 134, 1280, 84, AlloyUi.MiddleTop);
            void ShowSelected()
            {
                for (var i = selectedHolder.childCount - 1; i >= 0; i--) UnityEngine.Object.Destroy(selectedHolder.GetChild(i).gameObject);
                if (selected != null) ServerRect(selectedHolder, selected, 640, 42, AlloyUi.Middle, null);
            }
            ShowSelected();
            var list = AlloyUi.Node(r, "List").At(640, 218, 660, 409, AlloyUi.MiddleTop);
            list.gameObject.AddComponent<RectMask2D>();
            for (var i = 0; i < Servers.Count; i++)
            {
                var s = Servers[i];
                var x = i % 2 == 0 ? 165f : 495f;
                ServerRect(list, s, x, 10 + 74 * (i / 2), AlloyUi.MiddleTop, () =>
                {
                    selected = s;
                    SelectServer(s.Port);
                    ShowSelected();
                });
            }
            MenuBar(r, "back", 35, () => ShowTitle()).Place(640, 670, AlloyUi.Middle);
            return r.gameObject;
        }

        private static void ServerRect(RectTransform parent, ServerEntry s, float x, float y, Vector2 pivot, Action click)
        {
            var bg = AlloyUi.Rect(parent, "Server " + s.Name, AlloyUi.Hex(0x6b6b6b));
            bg.rectTransform.At(x, y, 320, 64, pivot);
            if (click != null)
            {
                var h = bg.gameObject.AddComponent<HoverRect>();
                h.Background = bg;
                h.Normal = AlloyUi.Hex(0x6b6b6b);
                h.Hovered = AlloyUi.Hex(0x878787);
                h.Clicked = click;
            }
            AlloyUi.Text(bg.transform, s.Name, 22, FontType.Bold, null, 3).Place(10, 32, AlloyUi.MiddleLeft);
            var load = s.MaxPlayers > 0 ? (float)s.Players / s.MaxPlayers : 0f;
            var color = s.Players >= s.MaxPlayers && s.MaxPlayers > 0 ? AlloyUi.Hex(0xb41221) : load >= 0.75f ? AlloyUi.Hex(0xe4bd10) : AlloyUi.Hex(0x12964b);
            AlloyUi.Text(bg.transform, $"{s.Players} / {s.MaxPlayers}", 22, FontType.Bold, color, 3).Place(310, 32, AlloyUi.MiddleRight);
        }

        private GameObject BuildLegends()
        {
            var r = ScreenBase("LegendsScreen", ScreenKind.Other);
            // The original's legends screen is an empty placeholder with no way back but logout; a back button is added here.
            MenuBar(r, "back", 35, () => ShowTitle()).Place(640, 670, AlloyUi.Middle);
            return r.gameObject;
        }

        // ---- character list -------------------------------------------------------------------------------------------------

        private GameObject BuildCharacterList()
        {
            var r = ScreenBase("CharacterListScreen", ScreenKind.Other);
            if (Account == null)
            {
                MenuBar(r, "back", 35, () => ShowTitle()).Place(640, 670, AlloyUi.Middle);
                return r.gameObject;
            }
            AlloyUi.TextButton(r, Account.Name, 32, null, FontType.Bold, 0, AlloyUi.Muted).Place(640, 50, AlloyUi.Middle);
            AlloyUi.Rect(r, "Divider", AlloyUi.Hex(0x2B2B2B)).rectTransform.At(0, 100, 1280, 5, AlloyUi.LeftTop);

            // Gold and fame, right-aligned at the divider.
            var gold = AlloyUi.GameSprite(r, SheetAtlas.Shared?.Static("lofiObj3", 0xE1), true, "GoldIcon");
            gold.rectTransform.At(1265, 88, 16, 16, AlloyUi.RightBottom);
            var goldText = AlloyUi.Text(r, Account.Gold.ToString(), 24).Place(1244, 93, AlloyUi.RightBottom);
            Canvas.ForceUpdateCanvases();
            var fameX = 1244 - goldText.preferredWidth - 10;
            AlloyUi.GameSprite(r, SheetAtlas.Shared?.Static("lofiObj3", 0xE0), true, "FameIcon").rectTransform.At(fameX, 88, 16, 16, AlloyUi.RightBottom);
            AlloyUi.Text(r, Account.Fame.ToString(), 24).Place(fameX - 16 - 5, 93, AlloyUi.RightBottom);

            AlloyUi.TextButton(r, "Characters", 24, () => { }).Place(15, 75, AlloyUi.MiddleLeft);
            var grave = AlloyUi.TextButton(r, "Graveyard", 24, () => { }).Place(157, 75, AlloyUi.MiddleLeft);
            grave.color = new Color(1, 1, 1, 0.6f);

            // Scroll area: 6 columns of 200x200 cards, "New Character" last.
            var scroll = AlloyUi.Node(r, "Scroll").At(0, 105, 1280, 535, AlloyUi.LeftTop);
            scroll.gameObject.AddComponent<RectMask2D>();
            var scrollImage = scroll.gameObject.AddComponent<Image>();
            scrollImage.color = new Color(0, 0, 0, 0);
            var content = AlloyUi.Node(scroll, "Content");
            var chars = Account.Characters.OrderByDescending(c => c.Fame).ToList();
            for (var i = 0; i <= chars.Count; i++)
            {
                var card = AlloyUi.Rect(content, "Card", AlloyUi.Hex(0x2B2B2B, 0.7f));
                card.rectTransform.At(5 + i % 6 * 210, 12 + i / 6 * 210, 200, 200, AlloyUi.LeftTop);
                if (i < chars.Count) CharacterCard(card.rectTransform, chars[i]);
                else NewCharacterCard(card.rectTransform, Math.Max(0, Account.MaxCharacters - chars.Count));
            }
            var total = 12 + (chars.Count / 6 + 1) * 210;
            content.At(0, 0, 1280, total, AlloyUi.LeftTop);
            var sr = scroll.gameObject.AddComponent<ScrollRect>();
            sr.content = content;
            sr.horizontal = false;
            sr.movementType = ScrollRect.MovementType.Clamped;
            sr.scrollSensitivity = 26;
            if (total > 535)
            {
                var track = AlloyUi.Picture(r, AlloyUi.UiSprite("Ui/ScrollBar/ScrollBarBackground.png", 3), "ScrollTrack");
                track.type = Image.Type.Sliced;
                track.raycastTarget = true;
                track.rectTransform.At(1270, 105, 10, 535, AlloyUi.MiddleTop);
                var bar = track.gameObject.AddComponent<Scrollbar>();
                bar.direction = Scrollbar.Direction.BottomToTop;
                var handle = AlloyUi.Picture(track.transform, AlloyUi.UiSprite("Ui/ScrollBar/ScrollBarHandle.png", 6), "Handle");
                handle.type = Image.Type.Sliced;
                handle.raycastTarget = true;
                handle.rectTransform.Fill();
                bar.handleRect = handle.rectTransform;
                bar.targetGraphic = handle;
                sr.verticalScrollbar = bar;
            }

            MenuBar(r, "back", 35, () => ShowTitle()).Place(640 - 56 - 50, 670, AlloyUi.MiddleRight);
            MenuBar(r, "play", 55, () =>
            {
                var first = Account.Characters.FirstOrDefault();
                if (first == null) SetOverlay(BuildClassSelection);
                else PlayCharacter?.Invoke(first.CharacterId);
            }, pulse: true).Place(640, 670, AlloyUi.Middle);
            MenuBar(r, "classes", 35, () => SetOverlay(BuildClassSelection)).Place(640 + 56 + 50, 670, AlloyUi.MiddleLeft);
            CheckForFailure(r);
            return r.gameObject;
        }

        private void CharacterCard(RectTransform card, Runity.Client.CharacterInfo c)
        {
            var cls = _content.Class((ushort)c.ClassType);
            AlloyUi.Text(card, cls?.Id ?? $"Class {c.ClassType}", 18, FontType.Bold).Place(100, 17, AlloyUi.Middle);
            var maxed = c.StatsMaxed;
            var maxedColor = maxed >= 8 ? AlloyUi.Hex(0xFCDF00) : maxed > 0 ? AlloyUi.White : AlloyUi.Hex(0xCFCFCF);
            AlloyUi.Text(card, $"{maxed}/8", 16, FontType.Bold, maxedColor).Place(100, 35, AlloyUi.Middle);
            var portrait = AlloyUi.GameSprite(card, cls != null ? AlloyUi.Players(cls.AnimatedIndex) : null, true, "Portrait");
            portrait.rectTransform.At(100, 74, 50, 50, AlloyUi.Middle);
            AlloyUi.Text(card, c.Fame.ToString(), 16, FontType.Bold).Place(100, 113, AlloyUi.Middle);
            var star = AlloyUi.UiSprite("Ui/CharacterList/StarGraphic.png");
            for (var i = 0; i < 5; i++)
            {
                var s = AlloyUi.Picture(card, star, "Star");
                s.rectTransform.At(100 - 32 + i * 16, 133, 16, 16, AlloyUi.Middle);
                s.color = new Color(0.5f, 0.5f, 0.5f, 1f);  // stars are never earned in the original (GetStars returns 0)
            }
            AlloyUi.TextButton(card, "Play", 24, () => PlayCharacter?.Invoke(c.CharacterId)).Place(100, 176, AlloyUi.Middle);
        }

        private void NewCharacterCard(RectTransform card, int remaining)
        {
            AlloyUi.Text(card, "New Character", 18, FontType.Bold).Place(100, 17, AlloyUi.Middle);
            var classes = _content.Classes.ToList();
            var pick = classes.Count > 0 ? classes[UnityEngine.Random.Range(0, classes.Count)] : null;
            var silhouette = AlloyUi.GameSprite(card, pick != null ? AlloyUi.Players(pick.AnimatedIndex) : null, false, "Silhouette");
            silhouette.color = new Color(0, 0, 0, 0.5f);
            silhouette.rectTransform.At(100, 65, 50, 50, AlloyUi.Middle);
            AlloyUi.Text(card, $"{remaining} Character Slots", 18, FontType.Bold).Place(100, 115, AlloyUi.Middle);
            AlloyUi.Text(card, "Remaining", 18, FontType.Bold).Place(100, 134, AlloyUi.Middle);
            AlloyUi.TextButton(card, "Create", 24, () => SetOverlay(BuildClassSelection)).Place(100, 178, AlloyUi.Middle);
        }

        // ---- class selection (ClassContainer) -------------------------------------------------------------------------------

        private GameObject BuildClassSelection(RectTransform layer)
        {
            var r = AlloyUi.Node(layer, "ClassSelection").Fill();
            var classes = _content.Classes.OrderBy(c => c.AnimatedIndex).ThenBy(c => c.Type).ToList();
            var info = AlloyUi.Rect(r, "ClassInfo", AlloyUi.Hex(0x171717)).rectTransform.At(160, 50, 960, 380, AlloyUi.LeftTop);
            var wheel = r.gameObject.AddComponent<ClassWheel>();
            wheel.Init(r, info, classes, _content);
            AlloyUi.TextButton(r, "Forward", 50, () => wheel.Step(1), FontType.Normal).Place(75, 485, AlloyUi.LeftTop);
            AlloyUi.TextButton(r, "Back", 50, () => wheel.Step(-1), FontType.Normal).Place(75, 560, AlloyUi.LeftTop);
            AlloyUi.TextButton(r, "Cancel", 50, () => CloseOverlay(), FontType.Normal).Place(75, 650, AlloyUi.LeftTop);
            AlloyUi.TextButton(r, "Play", 50, () =>
            {
                if (wheel.Selected == null) return;
                CloseOverlay();
                CreateCharacter?.Invoke(wheel.Selected.Type);
            }, FontType.Normal).Place(1000, 360, AlloyUi.LeftTop);
            return r.gameObject;
        }
    }

    /// <summary>Keyboard use of a form or dialog: Tab / Shift+Tab move between its fields; Enter moves on to the next field, and from the
    /// last field (or with none focused) presses the default button. Does nothing while Blocked (a dialog open over the form).</summary>
    public sealed class FormKeys : MonoBehaviour
    {
        public InputField[] Fields = Array.Empty<InputField>();
        public Action Submit;
        public Func<bool> Blocked = () => false;
        private int _lastFocused = -1;

        private void Update()
        {
            var keys = UnityEngine.InputSystem.Keyboard.current;
            if (keys == null || Blocked() || DevConsole.HasKeyboard) return;
            var focused = Array.FindIndex(Fields, f => f.isFocused);
            // A field ends its own editing on Enter, possibly earlier this frame: then the field focused last frame is the one.
            var current = focused >= 0 ? focused : _lastFocused;
            _lastFocused = focused;
            if (keys.tabKey.wasPressedThisFrame && Fields.Length > 0)
            {
                var step = keys.shiftKey.isPressed ? -1 : 1;
                Focus(current < 0 ? (step > 0 ? 0 : Fields.Length - 1) : (current + step + Fields.Length) % Fields.Length);
            }
            else if (keys.enterKey.wasPressedThisFrame || keys.numpadEnterKey.wasPressedThisFrame)
            {
                if (current >= 0 && current < Fields.Length - 1) Focus(current + 1);
                else Submit?.Invoke();
            }
        }

        private void Focus(int index)
        {
            Fields[index].Select();
            Fields[index].ActivateInputField();
            _lastFocused = index;
        }
    }

    /// <summary>CharacterWheel + ClassInfo: every class on a 300x20 ellipse (front = selected), rotating over 400 ms; the info panel
    /// shows the selected class's name, description, a walking 120 px portrait and its 4 starting items.</summary>
    public sealed class ClassWheel : MonoBehaviour
    {
        private const float Start = 4.81f;
        private readonly List<Image> _sprites = new List<Image>();
        private List<ClassDef> _classes;
        private ContentCatalog _content;
        private RectTransform _info;
        private Image _portrait;
        private float _angle = Start;
        private float _from;
        private float _to = Start;
        private float _t = 1f;
        private int _index;
        private int _shown = -1;

        public ClassDef Selected => _classes.Count > 0 ? _classes[_index] : null;

        public void Init(RectTransform root, RectTransform info, List<ClassDef> classes, ContentCatalog content)
        {
            _classes = classes;
            _content = content;
            _info = info;
            var ring = AlloyUi.Node(root, "Wheel").Fill();
            foreach (var c in classes)
            {
                var img = AlloyUi.GameSprite(ring, AlloyUi.Players(c.AnimatedIndex), true, c.Id);
                img.rectTransform.anchorMin = img.rectTransform.anchorMax = new Vector2(0, 1);
                img.rectTransform.sizeDelta = new Vector2(160, 160);
                img.rectTransform.pivot = new Vector2(0.5f, 0.5f);
                _sprites.Add(img);
            }
            Layout();
            ShowInfo();
        }

        public void Step(int delta)
        {
            if (_classes.Count == 0) return;
            _index = (_index + delta + _classes.Count) % _classes.Count;
            var step = Mathf.PI * 2 / _classes.Count;
            var target = Start - _index * step;
            // Shortest way round (the original spins the long way when wrapping).
            while (target - _angle > Mathf.PI) target -= Mathf.PI * 2;
            while (_angle - target > Mathf.PI) target += Mathf.PI * 2;
            _from = _angle;
            _to = target;
            _t = 0;
            ShowInfo();
        }

        private void Update()
        {
            if (_t < 1f)
            {
                _t = Mathf.Min(1f, _t + Time.unscaledDeltaTime / 0.4f);
                _angle = Mathf.Lerp(_from, _to, _t);
                Layout();
            }
            if (_portrait != null && Selected != null)
            {
                var frame = 1 + (int)(Time.unscaledTime * 1000 / 250) % 2;
                _portrait.sprite = AlloyUi.Players(Selected.AnimatedIndex, Facing.Down, frame) ?? AlloyUi.Players(Selected.AnimatedIndex);
            }
        }

        private void Layout()
        {
            var n = _sprites.Count;
            if (n == 0) return;
            var step = Mathf.PI * 2 / n;
            const float baseY = 610;
            var order = new List<(float y, Image img)>();
            for (var i = 0; i < n; i++)
            {
                // Original: centre (610 + 300 cos a, 530 - 20 sin a); alpha 1 - |20 sin a + 30| / 40, the selected class always 1.
                var a = _angle + i * step;
                var x = 610 + 300 * Mathf.Cos(a);
                var y = baseY - 80 - 20 * Mathf.Sin(a);
                var img = _sprites[i];
                img.rectTransform.anchoredPosition = new Vector2(x, -y);
                var alpha = i == _index ? 1f : Mathf.Clamp01(1f - Mathf.Abs(20 * Mathf.Sin(a) + 30) / 40f);
                img.color = new Color(1, 1, 1, alpha);
                order.Add((y, img));
            }
            foreach (var (_, img) in order.OrderBy(o => o.y)) img.transform.SetAsLastSibling();
        }

        private void ShowInfo()
        {
            if (_shown == _index || Selected == null) return;
            _shown = _index;
            for (var i = _info.childCount - 1; i >= 0; i--) Destroy(_info.GetChild(i).gameObject);
            var c = Selected;
            _portrait = AlloyUi.GameSprite(_info, AlloyUi.Players(c.AnimatedIndex), true, "Portrait");
            _portrait.rectTransform.At(420, 60, 120, 120, AlloyUi.LeftTop);
            AlloyUi.Text(_info, c.Id, 30, FontType.Bold).Place(480, 20, AlloyUi.Middle);
            AlloyUi.Text(_info, c.Description ?? "", 20, FontType.Bold, null, 0, 200).Place(100, 60, AlloyUi.LeftTop);
            EquipmentGrid.Build(_info, 368, 200, c.Equipment.Take(4).ToList(), c.SlotTypes.Take(4).ToList(), _content);
        }
    }
}

using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.UI;
using Runity.Client;
using Runity.Domain.Content;
using Runity.Domain.World;
using Runity.Presentation.Art;
using Runity.Protocol;

namespace Runity.Presentation.UI.Alloy
{
    /// <summary>The original client's in-game HUD (AlloyClient Game/Components/Hud): a 240 px panel on the right (minimap, portrait and
    /// name, XP / HP / MP bars, equipment, inventory / stats tabs, the loot bag or portal next to the player) and the chat box at the
    /// bottom left. Items are dragged between slots, onto a bag or onto the world (drop); double-click or Shift+click uses one.
    /// Everything shown comes from the client world; every action is a request the server checks.</summary>
    public sealed class AlloyHud
    {
        public const float Width = 240;

        public event Action<string> ChatSubmitted;

        private readonly ContentCatalog _content;
        private readonly RectTransform _root;
        private readonly RectTransform _panel;
        private readonly Minimap _minimap;
        private readonly Image _portrait;
        private readonly Text _name;
        private readonly StatusBar _xp;
        private readonly StatusBar _hp;
        private readonly StatusBar _mp;
        private readonly List<ItemTile> _equipment;
        private readonly List<ItemTile> _inventory = new List<ItemTile>();
        private readonly List<ItemTile> _bag = new List<ItemTile>();
        private readonly GameObject _inventoryPage;
        private readonly GameObject _statsPage;
        private readonly Image _inventoryTab;
        private readonly Image _statsTab;
        private readonly Dictionary<string, Text> _statValues = new Dictionary<string, Text>();
        private readonly RectTransform _interact;
        private readonly GameObject _bagPanel;
        private readonly GameObject _portalPanel;
        private readonly Text _portalName;
        private readonly ChatBox _chat;
        private readonly Text _overlay;
        private GameSession _session;
        private ClientWorld _world;
        private ClientEntity _bagEntity;
        private ClientEntity _portal;
        private ushort _classShown;

        public bool ChatFocused => _chat.Focused;

        public AlloyHud(Canvas canvas, ContentCatalog content)
        {
            _content = content;
            _root = AlloyUi.Node(canvas.transform, "Hud").Fill();

            _overlay = AlloyUi.Text(_root, "", 28, FontType.Bold, null, 4);
            var ov = _overlay.rectTransform;
            ov.anchorMin = ov.anchorMax = new Vector2(0.5f, 0.5f);
            ov.pivot = new Vector2(0.5f, 0.5f);
            ov.anchoredPosition = new Vector2(-Width / 2, 0);

            _chat = new ChatBox(_root);
            _chat.Submitted += t => ChatSubmitted?.Invoke(t);

            // The right panel: 240 x 720, glued to the right edge and centred vertically.
            var panel = AlloyUi.Rect(_root, "RightPanel", AlloyUi.Hex(0x363636));
            _panel = panel.rectTransform;
            _panel.anchorMin = _panel.anchorMax = new Vector2(1, 0.5f);
            _panel.pivot = new Vector2(1, 0.5f);
            _panel.sizeDelta = new Vector2(Width, 720);
            _panel.anchoredPosition = Vector2.zero;
            panel.raycastTarget = true;

            _minimap = new Minimap(_panel, 5, 5);

            // Character details: 30 px portrait, name 25 Bolder #DADADA.
            var details = AlloyUi.Node(_panel, "Details").At(5, 235, 230, 34, AlloyUi.LeftTop);
            _portrait = AlloyUi.GameSprite(details, null, true, "Portrait");
            _portrait.rectTransform.At(4, 4, 30, 30, AlloyUi.LeftTop);
            _name = AlloyUi.Text(details, "", 25, FontType.Bolder, AlloyUi.Hex(0xDADADA)).Place(40, 20, AlloyUi.MiddleLeft);

            const float barsY = 274;
            _xp = new StatusBar(_panel, 15, barsY, AlloyUi.Hex(0x5A8025), "Lvl 1");
            _hp = new StatusBar(_panel, 15, barsY + 28, AlloyUi.Hex(0xE03434), "HP");
            _mp = new StatusBar(_panel, 15, barsY + 56, AlloyUi.Hex(0x6084E0), "MP");

            const float equipY = barsY + 84;
            _equipment = EquipmentGrid.Build(_panel, 12, equipY, new List<int>(), new List<int>(), content);
            for (var i = 0; i < _equipment.Count; i++) Wire(_equipment[i], i, own: true);

            // Tabs (inventory / stats) above a 223 x 148 page.
            const float tabsY = equipY + 56 + 32;
            _inventoryTab = Tab(_panel, 0, tabsY, 24, () => SelectTab(inventory: true));
            _statsTab = Tab(_panel, 1, tabsY, 25, () => SelectTab(inventory: false));

            var invBg = AlloyUi.Rect(_panel, "InventoryPage", AlloyUi.Hex(0x242222));
            invBg.rectTransform.At(8, tabsY, 223, 148, AlloyUi.LeftTop);
            _inventoryPage = invBg.gameObject;
            for (var i = 0; i < 8; i++)
            {
                var tile = ItemTile.Create(invBg.transform, i % 4 * 53 + 8, i / 4 * 53 + 8, 48, content);
                tile.Background.color = AlloyUi.Hex(0x545454);
                tile.SlotNumber = i + 1;
                Wire(tile, 4 + i, own: true);
                _inventory.Add(tile);
            }
            // Potion quick slots (empty boxes in the original too).
            AlloyUi.Rect(invBg.transform, "HpPotions", AlloyUi.Hex(0x3E3D3D)).rectTransform.At(8, 114, 101, 26, AlloyUi.LeftTop);
            AlloyUi.Rect(invBg.transform, "HpPotionsInner", AlloyUi.Hex(0x242222)).rectTransform.At(11, 117, 95, 20, AlloyUi.LeftTop);
            AlloyUi.Rect(invBg.transform, "MpPotions", AlloyUi.Hex(0x3E3D3D)).rectTransform.At(114, 114, 101, 26, AlloyUi.LeftTop);
            AlloyUi.Rect(invBg.transform, "MpPotionsInner", AlloyUi.Hex(0x242222)).rectTransform.At(117, 117, 95, 20, AlloyUi.LeftTop);

            var statsBg = AlloyUi.Rect(_panel, "StatsPage", AlloyUi.Hex(0x242222));
            statsBg.rectTransform.At(8, tabsY, 224, 150, AlloyUi.LeftTop);
            _statsPage = statsBg.gameObject;
            string[] stats = { "ATK", "DEF", "SPD", "DEX", "VIT", "WIS" };
            for (var i = 0; i < stats.Length; i++)
            {
                var x = i % 2 == 0 ? 40f : 128f;
                var y = 49f + i / 2 * 26f;
                var label = AlloyUi.Text(statsBg.transform, stats[i], 16).Place(x, y, AlloyUi.MiddleLeft);
                Canvas.ForceUpdateCanvases();
                _statValues[stats[i]] = AlloyUi.Text(statsBg.transform, "0", 16, FontType.Bold, AlloyUi.Hex(0xFFC800))
                    .Place(x + label.preferredWidth + 5, y, AlloyUi.MiddleLeft);
            }
            SelectTab(inventory: true);

            // Interaction panel: the loot bag or the portal within reach.
            _interact = AlloyUi.Node(_panel, "Interact").At(8, 605, 218, 110, AlloyUi.LeftTop);
            _bagPanel = AlloyUi.Node(_interact, "Bag").Fill().gameObject;
            for (var i = 0; i < 8; i++)
            {
                var tile = ItemTile.Create(_bagPanel.transform, i % 4 * 53 + 8, i / 4 * 53 + 8, 48, content);
                tile.Background.color = AlloyUi.Hex(0x545454);
                Wire(tile, i, own: false);
                _bag.Add(tile);
            }
            _portalPanel = AlloyUi.Node(_interact, "Portal").Fill().gameObject;
            _portalName = AlloyUi.Text(_portalPanel.transform, "", 22, FontType.Bold).Place(109, 16, AlloyUi.MiddleTop);
            AlloyUi.TextButton(_portalPanel.transform, "Enter", 20, EnterPortal).Place(109, 70, AlloyUi.MiddleTop);
            _bagPanel.SetActive(false);
            _portalPanel.SetActive(false);
        }

        private Image Tab(RectTransform parent, int index, float pageY, int icon, Action click)
        {
            var bg = AlloyUi.Rect(parent, "Tab" + index, AlloyUi.Hex(0x6B6A6A));
            bg.rectTransform.At(6 + 8 + index * 40, pageY - 24, 34, 24, AlloyUi.LeftTop);
            bg.raycastTarget = true;
            var h = bg.gameObject.AddComponent<HoverRect>();
            h.Clicked = click;
            var img = AlloyUi.GameSprite(bg.transform, SheetAtlas.Shared?.Static("lofiInterfaceBig", icon), false, "Icon");
            img.rectTransform.At(5, 0, 24, 24, AlloyUi.LeftTop);
            return bg;
        }

        private void SelectTab(bool inventory)
        {
            _inventoryPage.SetActive(inventory);
            _statsPage.SetActive(!inventory);
            _inventoryTab.color = AlloyUi.Hex(inventory ? 0x242222u : 0x6B6A6Au);
            _statsTab.color = AlloyUi.Hex(inventory ? 0x6B6A6Au : 0x242222u);
            foreach (var hr in new[] { _inventoryTab.GetComponent<HoverRect>(), _statsTab.GetComponent<HoverRect>() })
            {
                hr.Normal = hr.Background != null ? hr.Background.color : Color.clear;
            }
        }

        private void Wire(ItemTile tile, int slot, bool own)
        {
            tile.Slot = slot;
            tile.Own = own;
            tile.Dropped = OnDropped;
            tile.DroppedOnWorld = t => { if (t.Own) _session?.DropItem(t.Slot); };
            tile.Used = t => { if (t.Own) _session?.UseItem(t.Slot); };
        }

        private uint OwnerOf(ItemTile t) => t.Own ? _world?.LocalEntityId ?? 0 : _bagEntity?.Id ?? 0;

        private void OnDropped(ItemTile from, ItemTile to)
        {
            if (_session == null || from == to) return;
            _session.MoveItem(OwnerOf(from), from.Slot, OwnerOf(to), to.Slot);
        }

        private void EnterPortal()
        {
            if (_portal != null) _session?.UsePortal(_portal.Id);
        }

        public void Show(bool visible)
        {
            _root.gameObject.SetActive(visible);
            if (!visible) ItemTooltip.Hide(null);
        }

        public void Attach(GameSession session)
        {
            _session = session;
            _world = session?.World;
            _classShown = 0;
            _chat.Attach(_world);
            _minimap.Attach(_world, _content);
        }

        public void SetOverlay(string text) => _overlay.text = text ?? "";

        public void FocusChat() => _chat.Open();

        /// <summary>Per frame: copies the world into the HUD.</summary>
        public void Refresh(ClientWorld world, GameSession session)
        {
            if (_world != world) Attach(session);
            _chat.Tick();
            if (world == null || !world.InWorld) return;
            var me = world.LocalPlayer;
            var cls = me != null ? _content.Class(me.ObjectType) : null;
            if (me != null && me.ObjectType != _classShown)
            {
                _classShown = me.ObjectType;
                _portrait.sprite = cls != null ? AlloyUi.Players(cls.AnimatedIndex, Facing.Right) : null;
                _portrait.enabled = _portrait.sprite != null;
                for (var i = 0; i < _equipment.Count; i++) _equipment[i].SlotType = cls != null && i < cls.SlotTypes.Count ? cls.SlotTypes[i] : 0;
            }
            if (me != null) _name.text = me.Name;
            var s = world.Stats;
            if (s != null)
            {
                if (s.Level >= 20) _xp.Set("Fame", AlloyUi.Hex(0xE25F00), s.Fame, 0);
                else _xp.Set("Lvl " + s.Level, AlloyUi.Hex(0x5A8025), s.Xp, s.XpGoal);
                _hp.Set("HP", null, s.Hp, s.MaxHp);
                _mp.Set("MP", null, s.Mp, s.MaxMp);
                _statValues["ATK"].text = s.Attack.ToString();
                _statValues["DEF"].text = s.Defense.ToString();
                _statValues["SPD"].text = s.Speed.ToString();
                _statValues["DEX"].text = s.Dexterity.ToString();
                _statValues["VIT"].text = s.Vitality.ToString();
                _statValues["WIS"].text = s.Wisdom.ToString();
            }
            var items = world.Items;
            for (var i = 0; i < _equipment.Count; i++) _equipment[i].Set(i < items.Count ? items[i] : -1, _equipment[i].SlotType, cls);
            for (var i = 0; i < _inventory.Count; i++) _inventory[i].Set(4 + i < items.Count ? items[4 + i] : -1, 0, cls);

            _bagEntity = session?.NearestBag();
            _portal = _bagEntity == null ? session?.NearestPortal(1.8f) : null;
            _bagPanel.SetActive(_bagEntity != null);
            _portalPanel.SetActive(_portal != null);
            if (_bagEntity != null)
            {
                for (var i = 0; i < _bag.Count; i++) _bag[i].Set(i < _bagEntity.Items.Count ? _bagEntity.Items[i] : -1, 0, cls);
            }
            if (_portal != null)
            {
                var def = _content.Object(_portal.ObjectType);
                var label = !string.IsNullOrEmpty(_portal.Name) ? _portal.Name : def?.DisplayName ?? def?.Id ?? "Portal";
                _portalName.text = label.StartsWith("Locked ") ? label.Substring(7) : label;
            }
            _minimap.Refresh(world);
        }
    }

    /// <summary>StatusBar: the bar3 9-slice in #545454 behind a fill tinted with the bar colour, a label at the left (16 Bolder) and
    /// "value/max" centred (16 Bold), both white.</summary>
    public sealed class StatusBar
    {
        private const float W = 210;
        private const float H = 20;
        private readonly Image _fill;
        private readonly Text _label;
        private readonly Text _value;

        public StatusBar(RectTransform parent, float x, float y, Color color, string label)
        {
            var sprite = AlloyUi.UiSprite("Ui/bar3.png", 6);
            var back = AlloyUi.Picture(parent, sprite, "Bar " + label);
            back.type = Image.Type.Sliced;
            back.pixelsPerUnitMultiplier = 0.6f;  // source border 6 px drawn at 10 px, as the original's CutX/CutY
            back.color = AlloyUi.Hex(0x545454);
            back.rectTransform.At(x, y, W, H, AlloyUi.LeftTop);
            _fill = AlloyUi.Picture(back.transform, sprite, "Fill");
            _fill.type = Image.Type.Sliced;
            _fill.pixelsPerUnitMultiplier = 0.6f;
            _fill.color = color;
            _fill.rectTransform.At(0, 0, W, H, AlloyUi.LeftTop);
            _label = AlloyUi.Text(back.transform, label, 16, FontType.Bolder, null, 1).Place(4, H / 2, AlloyUi.MiddleLeft);
            _value = AlloyUi.Text(back.transform, "", 16, FontType.Bold, null, 1).Place(W / 2, H / 2, AlloyUi.Middle);
        }

        public void Set(string label, Color? color, long value, long max)
        {
            _label.text = label;
            if (color is { } c) _fill.color = c;
            var f = max > 0 ? Mathf.Clamp01(value / (float)max) : 1f;
            _fill.rectTransform.sizeDelta = new Vector2((int)(W * f), H);
            _fill.enabled = f > 0;
            _value.text = max > 0 ? $"{value}/{max}" : value.ToString();
        }
    }

    /// <summary>The minimap: one texel per tile in its dominant colour (wall tops, occupying objects over the ground), a 230 px view
    /// around the player that does not turn with the camera, and dots for players (#FFFF00), enemies (#FF0000) and portals (#0000FF).
    /// Mouse wheel over it zooms (1..max, step size/1280 per notch).</summary>
    public sealed class Minimap
    {
        private const float Size = 230;
        private readonly RawImage _view;
        private readonly RectTransform _dots;
        private readonly List<Image> _pool = new List<Image>();
        private ClientWorld _world;
        private ContentCatalog _content;
        private Texture2D _texture;
        private Color32[] _pixels;
        private int _w;
        private int _h;
        private bool _dirty;
        private float _uploadAt;
        private float _zoom = 4f;

        public Minimap(RectTransform parent, float x, float y)
        {
            var bg = AlloyUi.Rect(parent, "Minimap", Color.black);
            bg.rectTransform.At(x, y, Size, Size, AlloyUi.LeftTop);
            bg.gameObject.AddComponent<RectMask2D>();
            bg.raycastTarget = true;
            bg.gameObject.AddComponent<MinimapWheel>().Wheel = d => _zoom = Mathf.Clamp(_zoom + d * Mathf.Max(_w, _h) / 1280f, 1f, Mathf.Max(1f, Mathf.Max(_w, _h) / 32f));
            var view = AlloyUi.Node(bg.transform, "Tiles").Fill();
            _view = view.gameObject.AddComponent<RawImage>();
            _view.raycastTarget = false;
            _dots = AlloyUi.Node(bg.transform, "Dots").Fill();
            var arrow = AlloyUi.Rect(bg.transform, "Me", new Color(0.2f, 0.4f, 1f));
            arrow.rectTransform.At(Size / 2, Size / 2, 6, 6, AlloyUi.Middle);
        }

        public void Attach(ClientWorld world, ContentCatalog content)
        {
            if (_world != null)
            {
                _world.WorldChanged -= OnWorld;
                _world.TilesReceived -= OnTiles;
            }
            _world = world;
            _content = content;
            if (_world == null) return;
            _world.WorldChanged += OnWorld;
            _world.TilesReceived += OnTiles;
            if (_world.Info != null) OnWorld(_world.Info);
        }

        private void OnWorld(WorldInfo info)
        {
            _w = Mathf.Clamp(info.Width, 1, 4096);
            _h = Mathf.Clamp(info.Height, 1, 4096);
            if (_texture != null) UnityEngine.Object.Destroy(_texture);
            _texture = new Texture2D(_w, _h, TextureFormat.RGBA32, false) { filterMode = FilterMode.Point, wrapMode = TextureWrapMode.Clamp };
            _pixels = new Color32[_w * _h];
            for (var i = 0; i < _pixels.Length; i++) _pixels[i] = new Color32(0, 0, 0, 255);
            _texture.SetPixels32(_pixels);
            _texture.Apply();
            _view.texture = _texture;
        }

        private void OnTiles(IReadOnlyList<TileUpdate> tiles)
        {
            if (_pixels == null || SheetAtlas.Shared == null) return;
            foreach (var t in tiles)
            {
                if (t.X >= _w || t.Y >= _h) continue;
                Color32 c = default;
                var obj = t.ObjectType != 0 ? _content.Object(t.ObjectType) : null;
                if (obj != null && (obj.Static && obj.OccupySquare || obj.Class == "Wall"))
                    c = SheetAtlas.Shared.Dominant(obj.TopArtKey ?? obj.ArtKey);
                if (c.a == 0)
                {
                    var g = _content.Ground(t.GroundType);
                    c = SheetAtlas.Shared.Dominant(g?.RandomArtKeys != null ? g.RandomArtKeys[0] : g?.ArtKey);
                }
                if (c.a == 0) c = new Color32(0, 0, 0, 255);
                _pixels[(_h - 1 - t.Y) * _w + t.X] = c;
            }
            _dirty = true;
        }

        public void Refresh(ClientWorld world)
        {
            if (_texture == null) return;
            if (_dirty && Time.unscaledTime >= _uploadAt)
            {
                _texture.SetPixels32(_pixels);
                _texture.Apply();
                _dirty = false;
                _uploadAt = Time.unscaledTime + 0.25f;
            }
            var me = world.Predictor.RenderPosition();
            var half = Size / _zoom / 2f;
            _view.uvRect = new Rect((me.X - half) / _w, (_h - me.Y - half) / _h, 2 * half / _w, 2 * half / _h);
            var n = 0;
            foreach (var e in world.Entities.Values)
            {
                if (e.Id == world.LocalEntityId || n >= 1000) continue;
                var def = _content.Object(e.ObjectType);
                Color color;
                if (e.Kind == EntityKind.Player) color = AlloyUi.Hex(0xFFFF00);
                else if (def != null && def.Enemy) color = AlloyUi.Hex(0xFF0000);
                else if (e.Kind == EntityKind.Portal || def?.Class == "Portal") color = AlloyUi.Hex(0x0000FF);
                else continue;
                var dx = (e.RenderPosition.X - me.X) / half;
                var dy = (e.RenderPosition.Y - me.Y) / half;
                if (Mathf.Abs(dx) > 1 || Mathf.Abs(dy) > 1) continue;
                var dot = n < _pool.Count ? _pool[n] : NewDot();
                dot.enabled = true;
                dot.color = color;
                dot.rectTransform.anchoredPosition = new Vector2(Size / 2 + Size / 2 * dx, -(Size / 2 + Size / 2 * dy));
                n++;
            }
            for (var i = n; i < _pool.Count; i++) _pool[i].enabled = false;
        }

        private Image NewDot()
        {
            var d = AlloyUi.Rect(_dots, "Dot", Color.white);
            d.raycastTarget = false;
            d.rectTransform.At(0, 0, 6.5f, 6.5f, AlloyUi.Middle);
            _pool.Add(d);
            return d;
        }

        private sealed class MinimapWheel : MonoBehaviour, IScrollHandler
        {
            public Action<float> Wheel;
            public void OnScroll(PointerEventData e) => Wheel?.Invoke(Mathf.Sign(e.scrollDelta.y));
        }
    }

    /// <summary>The chat box: the newest 7 lines at the bottom left (18 Bold, black outline), each hidden 20 s after it arrived unless
    /// history is open (PageUp shows / scrolls back 7 lines, PageDown forward); Enter opens the input, Enter again sends.</summary>
    public sealed class ChatBox
    {
        private const float MaxWidth = 640;
        private const float MaxHeight = 358;
        private const int MaxLines = 7;
        private readonly RectTransform _root;
        private readonly List<Text> _lines = new List<Text>();
        private readonly List<(ChatLine Line, float Time)> _history = new List<(ChatLine, float)>();
        private readonly InputField _input;
        private ClientWorld _world;
        private bool _showAll;
        private int _offset;
        private float _nextRefresh;

        public event Action<string> Submitted;
        public bool Focused => _input.gameObject.activeSelf && _input.isFocused;

        public ChatBox(RectTransform parent)
        {
            _root = AlloyUi.Node(parent, "Chat");
            _root.anchorMin = _root.anchorMax = Vector2.zero;
            _root.pivot = Vector2.zero;
            _root.sizeDelta = new Vector2(MaxWidth, MaxHeight + 34);
            _root.anchoredPosition = Vector2.zero;
            for (var i = 0; i < MaxLines; i++)
            {
                var t = AlloyUi.Text(_root, "", 18, FontType.Bold, null, 3, MaxWidth - 6);
                t.supportRichText = true;
                t.rectTransform.anchorMin = t.rectTransform.anchorMax = new Vector2(0, 1);
                t.rectTransform.pivot = new Vector2(0, 0);
                _lines.Add(t);
            }
            var box = AlloyUi.Picture(_root, AlloyUi.UiSprite("Ui/TextBox.png", 2), "Input");
            box.type = Image.Type.Sliced;
            box.raycastTarget = true;
            box.rectTransform.At(0, MaxHeight + 2, MaxWidth, 28, AlloyUi.LeftTop);
            _input = box.gameObject.AddComponent<InputField>();
            var text = AlloyUi.Text(box.transform, "", 18, FontType.Bold, null, 3);
            UnityEngine.Object.Destroy(text.GetComponent<ContentSizeFitter>());
            text.alignment = TextAnchor.MiddleLeft;
            text.rectTransform.Fill();
            text.rectTransform.offsetMin = new Vector2(6, 0);
            text.rectTransform.offsetMax = new Vector2(-6, 0);
            _input.textComponent = text;
            _input.lineType = InputField.LineType.SingleLine;
            _input.characterLimit = 128;
            _input.caretColor = Color.white;
            _input.customCaretColor = true;
            _input.onSubmit.AddListener(OnSubmit);
            box.gameObject.SetActive(false);
        }

        public void Attach(ClientWorld world)
        {
            if (_world != null) _world.ChatReceived -= OnChat;
            _world = world;
            _history.Clear();
            _offset = 0;
            _showAll = false;
            if (_world != null) _world.ChatReceived += OnChat;
            Layout();
        }

        private void OnChat(ChatLine line)
        {
            _history.Add((line, Time.unscaledTime));
            if (_history.Count > 100) _history.RemoveAt(0);
            Layout();
        }

        public void Open()
        {
            _input.gameObject.SetActive(true);
            _input.text = "";
            _input.ActivateInputField();
        }

        private void OnSubmit(string text)
        {
            if (!string.IsNullOrWhiteSpace(text)) Submitted?.Invoke(text.Trim());
            _input.text = "";
            _input.DeactivateInputField();
            _input.gameObject.SetActive(false);
        }

        public void Tick()
        {
            var kb = UnityEngine.InputSystem.Keyboard.current;
            if (kb != null)
            {
                if (kb.pageUpKey.wasPressedThisFrame)
                {
                    if (!_showAll) _showAll = true;
                    else _offset = Mathf.Min(_offset + MaxLines, Mathf.Max(0, _history.Count - MaxLines));
                    Layout();
                }
                if (kb.pageDownKey.wasPressedThisFrame)
                {
                    if (_offset == 0) _showAll = false;
                    _offset = Mathf.Max(0, _offset - MaxLines);
                    Layout();
                }
            }
            if (Time.unscaledTime >= _nextRefresh) Layout();
        }

        private void Layout()
        {
            _nextRefresh = Time.unscaledTime + 1f;
            var end = _history.Count - _offset;
            var bottom = MaxHeight;  // the newest line's bottom edge, measured down from the top of the chat area
            var slot = 0;
            for (var i = end - 1; i >= 0 && slot < MaxLines; i--)
            {
                var (line, time) = _history[i];
                if (!_showAll && Time.unscaledTime > time + 20f) break;
                var t = _lines[slot++];
                t.text = Format(line);
                t.enabled = true;
                Canvas.ForceUpdateCanvases();
                t.rectTransform.anchoredPosition = new Vector2(3, -bottom);
                bottom -= t.preferredHeight + 4;
            }
            for (var i = slot; i < _lines.Count; i++) _lines[i].enabled = false;
        }

        /// <summary>The original's colours: players "&lt;name&gt;" green + white text, server lines yellow, tells #00F0FF, guild #A6FF5D,
        /// errors red.</summary>
        private static string Format(ChatLine l)
        {
            string Esc(string s) => (s ?? "").Replace("<", "‹").Replace(">", "›");
            switch (l.Channel)
            {
                case ChatChannel.System: return $"<color=#FFFF00>{Esc(l.Text)}</color>";
                case ChatChannel.Error: return $"<color=#FF0000>{Esc(l.Text)}</color>";
                case ChatChannel.Whisper: return $"<color=#00FF00>‹{Esc(l.Sender)}›</color> <color=#00F0FF>{Esc(l.Text)}</color>";
                case ChatChannel.Guild: return $"<color=#A6FF5D>‹{Esc(l.Sender)}› {Esc(l.Text)}</color>";
                default:
                    if (string.IsNullOrEmpty(l.Sender)) return $"<color=#FFFF00>{Esc(l.Text)}</color>";
                    if (l.Sender.StartsWith("#")) return $"<color=#FFA800>‹{Esc(l.Sender.Substring(1))}›</color> {Esc(l.Text)}";
                    return $"<color=#00FF00>‹{Esc(l.Sender)}›</color> {Esc(l.Text)}";
            }
        }
    }
}

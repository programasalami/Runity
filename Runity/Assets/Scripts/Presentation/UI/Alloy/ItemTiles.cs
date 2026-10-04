using System;
using System.Collections.Generic;
using System.Text;
using Object = UnityEngine.Object;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.UI;
using Runity.Domain.Content;
using Runity.Presentation.Art;

namespace Runity.Presentation.UI.Alloy
{
    /// <summary>AlloyClient Ui/ItemConstants: slot types and the silhouette drawn in an empty slot of that type.</summary>
    public static class ItemConstants
    {
        private static readonly Dictionary<int, (string Sheet, int Index)> Silhouettes = new Dictionary<int, (string, int)>
        {
            [1] = ("lofiObj5", 48), [2] = ("lofiObj5", 96), [3] = ("lofiObj5", 80), [4] = ("lofiObj6", 80),
            [5] = ("lofiObj6", 112), [6] = ("lofiObj5", 0), [7] = ("lofiObj5", 32), [8] = ("lofiObj5", 64),
            [9] = ("lofiObj", 44), [11] = ("lofiObj6", 64), [12] = ("lofiObj6", 160), [13] = ("lofiObj6", 32),
            [14] = ("lofiObj5", 16), [15] = ("lofiObj6", 48), [16] = ("lofiObj6", 96), [17] = ("lofiObj5", 112),
            [18] = ("lofiObj6", 128), [19] = ("lofiObj6", 0), [20] = ("lofiObj6", 16), [21] = ("lofiObj6", 144),
            [22] = ("lofiObj6", 176), [23] = ("lofiObj6", 192), [24] = ("lofiObj3", 540), [25] = ("lofiObj3", 555),
        };

        public const int PotionType = 10;

        public static Sprite Slot(int slotType) =>
            Silhouettes.TryGetValue(slotType, out var s) ? SheetAtlas.Shared?.Static(s.Sheet, s.Index) : null;
    }

    /// <summary>One ItemTile: background (#454545 gear, #545454 inventory / bags, #5C1D1D when the class cannot use the item), the
    /// slot silhouette or number in #363636, the item icon (outlined), the tier tag ("T{n}" white, "UT" #8A2BE2) and the tooltip.
    /// Items are dragged (more than 3 px) onto another tile or the world; double-click or Shift+click uses them.</summary>
    public sealed class ItemTile : MonoBehaviour, IPointerEnterHandler, IPointerExitHandler, IPointerClickHandler, IBeginDragHandler,
        IDragHandler, IEndDragHandler
    {
        public Image Background;
        public Image Icon;
        public Text Tier;
        public Text Number;
        public int SlotType;
        public int ItemType = -1;
        public int Slot;
        public bool Own;
        /// <summary>1-8 on the player's empty inventory slots; 0 = none.</summary>
        public int SlotNumber;
        public Color NormalBackground;
        public Action<ItemTile, ItemTile> Dropped;
        public Action<ItemTile> DroppedOnWorld;
        public Action<ItemTile> Used;
        private ContentCatalog _content;
        private Image _ghost;
        private float _lastClick = -1f;

        public static ItemTile Create(Transform parent, float x, float y, float size, ContentCatalog content)
        {
            var bg = AlloyUi.Rect(parent, "ItemTile", AlloyUi.Hex(0x454545));
            bg.rectTransform.At(x, y, size, size, AlloyUi.LeftTop);
            var tile = bg.gameObject.AddComponent<ItemTile>();
            tile._content = content;
            tile.Background = bg;
            tile.Icon = AlloyUi.GameSprite(bg.transform, null, true, "Icon");
            tile.Icon.rectTransform.Fill();
            tile.Icon.rectTransform.offsetMin = new Vector2(size * 0.1f, size * 0.1f);
            tile.Icon.rectTransform.offsetMax = new Vector2(-size * 0.1f, -size * 0.1f);
            tile.Tier = AlloyUi.Text(bg.transform, "", 16, FontType.Bold, null, 6).Place(size - 2, size, AlloyUi.RightBottom);
            tile.Number = AlloyUi.Text(bg.transform, "", 32, FontType.Bold, AlloyUi.Hex(0x363636)).Place(size / 2, size / 2, AlloyUi.Middle);
            tile.Number.transform.SetSiblingIndex(0);
            tile.NormalBackground = bg.color;
            bg.raycastTarget = true;
            return tile;
        }

        /// <summary>Shows an item (or the empty slot). cls: the player's class, to mark items it cannot equip in red.</summary>
        public void Set(int itemType, int slotType, ClassDef cls)
        {
            Set(itemType, slotType);
            var def = _content?.Item(itemType);
            var unusable = def != null && cls != null && def.SlotType != 0 && def.SlotType != ItemConstants.PotionType && !def.Consumable &&
                           !cls.SlotTypes.Contains(def.SlotType);
            if (Background.color != AlloyUi.Hex(0x5C1D1D) && !unusable) NormalBackground = Background.color;
            Background.color = unusable ? AlloyUi.Hex(0x5C1D1D) : NormalBackground;
        }

        public void Set(int itemType, int slotType)
        {
            ItemType = itemType;
            SlotType = slotType;
            var def = _content?.Item(itemType);
            var outline = Icon.GetComponents<Shadow>();
            Number.text = def == null && SlotNumber > 0 ? SlotNumber.ToString() : "";
            if (def == null)
            {
                Icon.sprite = ItemConstants.Slot(slotType);
                Icon.enabled = Icon.sprite != null;
                Icon.color = AlloyUi.Hex(0x363636);
                foreach (var o in outline) o.enabled = false;
                Tier.text = "";
                return;
            }
            Icon.sprite = SheetAtlas.Shared?.Find(def.ArtKey);
            Icon.enabled = Icon.sprite != null;
            Icon.color = Color.white;
            foreach (var o in outline) o.enabled = true;
            var showTier = !def.Consumable && def.SlotType != ItemConstants.PotionType;
            Tier.text = !showTier ? "" : def.Tier >= 0 ? "T" + def.Tier : "UT";
            Tier.color = def.Tier >= 0 ? Color.white : AlloyUi.Hex(0x8A2BE2);
        }

        public void OnPointerEnter(PointerEventData e) => ItemTooltip.Show(_content?.Item(ItemType), this);
        public void OnPointerExit(PointerEventData e) => ItemTooltip.Hide(this);

        private void OnDisable()
        {
            ItemTooltip.Hide(this);
            EndGhost();
        }

        public void OnPointerClick(PointerEventData e)
        {
            if (e.button != PointerEventData.InputButton.Left || ItemType < 0 || e.dragging) return;
            var shift = UnityEngine.InputSystem.Keyboard.current?.shiftKey.isPressed ?? false;
            var doubled = Time.unscaledTime - _lastClick < 0.25f;
            _lastClick = Time.unscaledTime;
            if (shift || doubled) Used?.Invoke(this);
        }

        public void OnBeginDrag(PointerEventData e)
        {
            if (ItemType < 0 || e.button != PointerEventData.InputButton.Left) return;
            ItemTooltip.Hide(this);
            var canvas = GetComponentInParent<Canvas>().rootCanvas;
            _ghost = AlloyUi.GameSprite(canvas.transform, Icon.sprite, true, "Dragged item");
            _ghost.rectTransform.sizeDelta = Icon.rectTransform.rect.size;
            _ghost.rectTransform.anchorMin = _ghost.rectTransform.anchorMax = Vector2.zero;
            _ghost.rectTransform.pivot = new Vector2(0.5f, 0.5f);
            Icon.color = new Color(1, 1, 1, 0.35f);
            OnDrag(e);
        }

        public void OnDrag(PointerEventData e)
        {
            if (_ghost == null) return;
            var canvas = _ghost.canvas;
            _ghost.rectTransform.anchoredPosition = e.position / (canvas != null ? canvas.scaleFactor : 1f);
        }

        public void OnEndDrag(PointerEventData e)
        {
            if (_ghost == null) return;
            EndGhost();
            var target = e.pointerCurrentRaycast.gameObject != null ? e.pointerCurrentRaycast.gameObject.GetComponentInParent<ItemTile>() : null;
            if (target != null)
            {
                if (target != this) Dropped?.Invoke(this, target);
            }
            else if (e.pointerCurrentRaycast.gameObject == null)
            {
                DroppedOnWorld?.Invoke(this);  // released over the world, not over the HUD
            }
        }

        private void EndGhost()
        {
            if (_ghost == null) return;
            Destroy(_ghost.gameObject);
            _ghost = null;
            if (Icon != null && ItemType >= 0) Icon.color = Color.white;
        }
    }

    /// <summary>EquippedGrid: the 4 equipment slots on a 216x56 #676767 bar (tiles 49x49 at x = i*53+4, y = 3).</summary>
    public static class EquipmentGrid
    {
        public static List<ItemTile> Build(Transform parent, float x, float y, IList<int> items, IList<int> slotTypes, ContentCatalog content)
        {
            var bar = AlloyUi.Rect(parent, "EquippedGrid", AlloyUi.Hex(0x676767));
            bar.rectTransform.At(x, y, 216, 56, AlloyUi.LeftTop);
            var tiles = new List<ItemTile>();
            for (var i = 0; i < 4; i++)
            {
                var tile = ItemTile.Create(bar.transform, i * 53 + 4, 3, 49, content);
                tile.Set(i < items.Count ? items[i] : -1, i < slotTypes.Count ? slotTypes[i] : 0);
                tiles.Add(tile);
            }
            return tiles;
        }
    }

    /// <summary>EquipmentToolTip: 220 wide on the 9-slice tooltipBackgroundSmall (2 px #9B9B9B border, #363636 fill): 40 px icon,
    /// name Bold 16, tier tag, description and stat lines Normal 14 #AAAAAA. Anchored at the mouse, towards the screen centre.</summary>
    public static class ItemTooltip
    {
        private static RectTransform _box;
        private static Image _icon;
        private static Text _name;
        private static Text _tier;
        private static Text _body;
        private static Object _owner;

        public static void Show(ItemDef def, Object owner)
        {
            if (def == null) return;
            if (_box == null) Build();
            _owner = owner;
            _icon.sprite = SheetAtlas.Shared?.Find(def.ArtKey);
            _icon.enabled = _icon.sprite != null;
            _name.text = def.DisplayName;
            _tier.text = def.Consumable ? "" : def.Tier >= 0 ? "T" + def.Tier : "UT";
            _tier.color = def.Tier >= 0 ? Color.white : AlloyUi.Hex(0x8A2BE2);
            var sb = new StringBuilder();
            if (!string.IsNullOrEmpty(def.Description)) sb.Append(def.Description);
            foreach (var b in def.Bonuses) sb.Append('\n').Append(b.Key).Append(": ").Append(b.Value >= 0 ? "+" : "").Append(b.Value);
            if (def.HealAmount > 0) sb.Append("\nOn Consume: Heals ").Append(def.HealAmount).Append(" HP");
            if (def.MagicAmount > 0) sb.Append("\nOn Consume: Heals ").Append(def.MagicAmount).Append(" MP");
            _body.text = sb.ToString();
            Canvas.ForceUpdateCanvases();
            _box.sizeDelta = new Vector2(220, 48 + _body.preferredHeight + 10);
            _box.gameObject.SetActive(true);
            _box.SetAsLastSibling();
            Follow();
        }

        public static void Hide(Object owner)
        {
            if (_box != null && (owner == null || owner == _owner)) _box.gameObject.SetActive(false);
        }

        /// <summary>Called every frame while shown: keeps the tooltip at the mouse.</summary>
        public static void Follow()
        {
            if (_box == null || !_box.gameObject.activeSelf) return;
            var canvas = _box.GetComponentInParent<Canvas>();
            var mouse = UnityEngine.InputSystem.Mouse.current?.position.ReadValue() ?? Vector2.zero;
            var scale = canvas != null ? canvas.scaleFactor : 1f;
            var left = mouse.x < Screen.width / 2f;
            _box.pivot = left ? new Vector2(0, 0) : new Vector2(1, 0);
            var pos = mouse / scale + new Vector2(left ? 12 : -12, 12);
            var size = _box.sizeDelta;
            var screen = new Vector2(Screen.width, Screen.height) / scale;
            pos.x = left ? Mathf.Min(pos.x, screen.x - size.x) : Mathf.Max(pos.x, size.x);
            pos.y = Mathf.Min(pos.y, screen.y - size.y);
            _box.anchoredPosition = pos;
        }

        private static void Build()
        {
            var canvas = Object.FindAnyObjectByType<Canvas>();
            var bg = AlloyUi.Picture(canvas.transform, AlloyUi.UiSprite("Ui/ToolTip/TooltipBackgroundSmall.png", 9), "ItemTooltip");
            bg.type = Image.Type.Sliced;
            bg.pixelsPerUnitMultiplier = 1.8f;  // the original draws a 5 px screen border from the 9 px slice
            _box = bg.rectTransform;
            _box.anchorMin = _box.anchorMax = Vector2.zero;
            _box.gameObject.AddComponent<TooltipFollower>();
            _icon = AlloyUi.GameSprite(_box, null, true, "Icon");
            _icon.rectTransform.At(5, 5, 40, 40, AlloyUi.LeftTop);
            _name = AlloyUi.Text(_box, "", 16, FontType.Bold, null, 0, 140).Place(48, 25, AlloyUi.MiddleLeft);
            _tier = AlloyUi.Text(_box, "", 16, FontType.Bold, null, 6).Place(205, 25, AlloyUi.MiddleRight);
            _body = AlloyUi.Text(_box, "", 14, FontType.Normal, AlloyUi.Hex(0xAAAAAA), 0.5f, 204).Place(8, 48, AlloyUi.LeftTop);
        }

        private sealed class TooltipFollower : MonoBehaviour
        {
            private void LateUpdate() => Follow();
        }
    }
}

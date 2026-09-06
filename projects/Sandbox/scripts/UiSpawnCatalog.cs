using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The spawn menu's actual panel content: a Garry's-Mod-style layout - a category list
/// down the left, a search box, and a scrolling grid of large icon tiles on the right -
/// replacing the old small fixed 4-tab / 8-tile-paged panel. That panel was hard-capped
/// at 2 rows x 4 columns with nowhere for overflow to go and a tiny (560x420, a sliver of
/// a 2560x1440 screen) footprint; this one scrolls its content instead of paging it, so
/// growing the catalogue is purely a new <see cref="PropSpawner.Catalog"/> row (plus,
/// optionally, a <see cref="KnownCategories"/> entry) - this file's own construction code
/// does not change again.
/// </summary>
/// <remarks>
/// <para>
/// <b>Sibling of <c>SpawnMenu</c>, not its replacement.</b> <c>SpawnMenu</c> keeps owning
/// the state machine it always has - <see cref="SpawnMenu.IsOpen"/>, the Q-key toggle, the
/// authority gate - this only reads that public getter to know when to show, and calls
/// <see cref="PropSpawner.SpawnProp"/> directly on a tile click, preserving
/// <c>PropSpawner</c>'s "exactly one way a prop comes into the world" invariant.
/// </para>
/// <para>
/// <b>Icons vs. colour swatches.</b> <see cref="PropDef.IconPath"/> (added alongside the
/// icon-baking pass) drives each tile: set, it shows the baked icon texture; unset, it
/// falls back to the flat <see cref="PropDef.Color"/> swatch, with the name/mass label
/// always shown either way - an entry with no baked icon yet still reads clearly rather
/// than showing a blank or broken tile, which is what the user's original "mostly empty
/// grid" complaint was actually about.
/// </para>
/// <para>
/// <b>No true virtualization.</b> Every catalogue entry currently matching the active
/// category/search is instantiated as a real tile, capped at <see cref="MaxRenderedTiles"/>
/// (an overflow entry reports "N more - refine your search" instead of an unbounded
/// instantiation burst). That is fine at today's 37 entries and comfortably fine at a few
/// hundred, but a genuine "whole Kenney pack" import (Factory Kit alone enumerates over a
/// thousand models) would need real virtualization (only instantiate the rows actually
/// scrolled into view) - ponytail: known ceiling, upgrade path is windowed tile reuse
/// keyed by scroll offset instead of one tile GameObject per matching entry.
/// </para>
/// <para>
/// <b>Categories are a local lookup, not a new <see cref="PropDef"/> field</b> (same
/// reasoning as before: <c>PropSpawner.Catalog</c> is shared/contended elsewhere in this
/// project, so this keeps its own name-keyed table rather than growing the shared struct).
/// Updated for every current catalogue entry, including the Machinery/Construction/
/// Kenney-import rows the old table predated entirely - anything still unrecognised falls
/// back to "Misc" rather than being silently dropped.
/// </para>
/// </remarks>
public sealed class UiSpawnCatalog : EntityScript
{
    // ── Category lookup ──────────────────────────────────────────────────────────
    private static readonly string[] CategoryOrder =
    {
        "Primitives", "Crates", "Containers", "Furniture", "Machinery", "Construction", "Factory", "Misc",
    };

    private const string FallbackCategory = "Misc";

    private static readonly Dictionary<string, string> KnownCategories = new()
    {
        ["Small Block"] = "Primitives",
        ["Medium Block"] = "Primitives",
        ["Heavy Block"] = "Primitives",
        ["Marble"] = "Primitives",
        ["Ball"] = "Primitives",
        ["Heavy Ball"] = "Primitives",

        ["Small Wooden Crate"] = "Crates",
        ["Large Wooden Crate"] = "Crates",
        ["Long Wooden Crate"] = "Crates",
        ["Cardboard Box"] = "Crates",

        ["Barrel"] = "Containers",
        ["Trash Can"] = "Containers",

        ["Chair"] = "Furniture",
        ["Table"] = "Furniture",
        ["Bench"] = "Furniture",

        ["Large Cog"] = "Machinery",
        ["Medium Cog"] = "Machinery",
        ["Piston"] = "Machinery",
        ["Machine Block"] = "Machinery",
        ["Hopper"] = "Machinery",
        ["Pipe Segment"] = "Machinery",
        ["Arrow Sign"] = "Machinery",
        ["Warning Sign"] = "Machinery",

        ["Steel Plate"] = "Construction",
        ["Beam"] = "Construction",
        ["Wheel"] = "Construction",
        ["Hinge Plate"] = "Construction",
        ["Ball Joint"] = "Construction",
        ["Thruster Body"] = "Construction",

        ["Machine Fortified"] = "Factory",
        ["Catwalk Corner"] = "Factory",
        ["Catwalk Cross"] = "Factory",
        ["Catwalk Junction"] = "Factory",
        ["Catwalk Stairs Loop"] = "Factory",
        ["Catwalk Stairs"] = "Factory",
        ["Catwalk Straight"] = "Factory",

        ["Traffic Cone"] = "Misc",
    };

    // ── Layout constants (all relative to the panel's own top-left; the panel itself
    // is anchored to screen CENTRE - see OnAttach - so this is resolution-independent
    // even though every number below is a literal pixel, matching every other screen
    // in this project, e.g. UiMainMenu's centre-anchored buttons) ────────────────────
    private const float PanelW = 1400.0f;
    private const float PanelH = 860.0f;
    private const float OriginX = -PanelW / 2.0f;
    private const float OriginY = -PanelH / 2.0f;

    private const float SidebarW = 220.0f;
    private const float HeaderH = 56.0f;
    private const float SearchH = 36.0f;
    private const float ContentX = SidebarW + UiTheme.SpacingLarge;
    private const float ContentY = HeaderH + SearchH + UiTheme.SpacingLarge * 2.0f;
    private const float ContentW = PanelW - ContentX - UiTheme.SpacingLarge;
    private const float ContentH = PanelH - ContentY - UiTheme.SpacingLarge;

    private const float TileW = 170.0f;
    private const float TileH = 170.0f;
    private const float TileGap = 14.0f;
    private const int Columns = 6; // floor((ContentW + TileGap) / (TileW + TileGap)) at the constants above

    private const int MaxRenderedTiles = 300; // see class remarks: real virtualization is the upgrade path past this

    private readonly struct Tile
    {
        public readonly Entity Button;
        public readonly Entity Swatch;
        public readonly Entity Label;

        public Tile(Entity button, Entity swatch, Entity label)
        {
            Button = button;
            Swatch = swatch;
            Label = label;
        }
    }

    private Entity _canvas;
    private Entity _gridClip;
    private Entity _searchBox;
    private Entity _overflowLabel;
    private readonly List<Entity> _tabButtons = new();
    private readonly List<string> _categories = new();
    private readonly List<Tile> _tiles = new(); // pooled, grown/reused across refreshes - never shrunk (cheap idle cost, avoids per-refresh destroy/recreate churn)
    private readonly List<int> _tileCatalogIndex = new(); // catalogIndex shown by _tiles[i] this refresh, -1 for a pooled-but-unused tile

    private int _selectedCategory;
    private string _lastSearch = string.Empty;
    private float _scrollY;
    private bool _wasOpen;
    private bool _dirty = true; // forces one RefreshLayout on first open even if nothing "changed" yet

    public override void OnAttach()
    {
        _canvas = Ui.CreateCanvas();
        _canvas.MarkTransient(); // runtime UI, never save-worthy - see PhysicsGun.EnsureHud's own comment on why

        Entity dim = Ui.CreateImage(_canvas);
        Ui.SetAnchors(dim, Vector2.Zero, Vector2.One);
        Ui.SetOffsets(dim, Vector2.Zero, Vector2.Zero);
        Ui.SetImageColor(dim, new Vector4(0.0f, 0.0f, 0.0f, 0.45f));
        _dimOverlay = dim;

        Entity panel = Ui.CreateImage(_canvas);
        AnchorToPanel(panel, 0.0f, 0.0f, PanelW, PanelH);
        Ui.SetImageColor(panel, UiTheme.PanelBackground);
        Ui.SetImageCornerRadius(panel, UiTheme.PanelCornerRadius);

        Entity title = Ui.CreateText(_canvas, "Spawn Menu  (Q to close)");
        AnchorToPanel(title, UiTheme.SpacingLarge, UiTheme.Spacing, PanelW - UiTheme.SpacingLarge * 2.0f, 28.0f);
        Ui.SetFontSize(title, UiTheme.FontSizeBody);
        Ui.SetTextColor(title, UiTheme.TextColor);

        BuildSidebar();
        BuildSearchBox();
        BuildGridClip();

        Close();
    }

    private Entity _dimOverlay;

    /// <summary>Position an element by literal pixel offset from the PANEL's own
    /// top-left, using the screen-CENTRE point anchor every element here shares - the
    /// flat (non-nested) Ui system has no real parent/child rects, so every element's
    /// anchor is this same centre point and layout is simulated by all of them sharing
    /// <see cref="OriginX"/>/<see cref="OriginY"/> as their common top-left reference.</summary>
    private static void AnchorToPanel(Entity e, float x, float y, float w, float h)
    {
        Ui.SetAnchors(e, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(e, Vector2.Zero);
        Ui.SetRect(e, OriginX + x, OriginY + y, w, h);
    }

    /// <summary>Category list down the left side. Built once from
    /// <see cref="PropSpawner.Catalog"/> as it exists right now - a category is rare
    /// enough that adding one is worth touching this file; adding a PROP never is.</summary>
    private void BuildSidebar()
    {
        HashSet<string> present = new();
        foreach (PropDef def in PropSpawner.Catalog)
        {
            present.Add(CategoryOf(def.Name));
        }
        foreach (string category in CategoryOrder)
        {
            if (present.Contains(category))
            {
                _categories.Add(category);
            }
        }
        if (_categories.Count == 0)
        {
            _categories.Add(FallbackCategory);
        }

        const float rowH = 40.0f;
        for (int i = 0; i < _categories.Count; ++i)
        {
            Entity tab = Ui.CreateButton(_canvas);
            AnchorToPanel(tab, 0.0f, HeaderH + i * (rowH + UiTheme.Spacing), SidebarW - UiTheme.SpacingLarge, rowH);
            Ui.SetButtonLabel(tab, _categories[i]);
            _tabButtons.Add(tab);
        }
    }

    private void BuildSearchBox()
    {
        _searchBox = Ui.CreateTextBox(_canvas);
        AnchorToPanel(_searchBox, ContentX, HeaderH, ContentW, SearchH);
        Ui.SetPlaceholder(_searchBox, "Search props...");
        Ui.SetFontSize(_searchBox, UiTheme.FontSizeBody);
    }

    /// <summary>The clipped scroll viewport (<see cref="Ui.SetClip"/>, this project's
    /// RectMask2D equivalent). Tiles are real ECS children of this entity (see
    /// CreateTile's own comment) so the clip actually masks them, and are positioned in
    /// its local space, so scrolling is just moving each tile's local Y by -scrollY -
    /// see <see cref="RepositionTiles"/>.</summary>
    private void BuildGridClip()
    {
        _gridClip = Ui.CreateImage(_canvas);
        AnchorToPanel(_gridClip, ContentX, ContentY, ContentW, ContentH);
        Ui.SetImageColor(_gridClip, Vector4.Zero); // invisible - exists only to own the clip rect
        Ui.SetClip(_gridClip, true);

        _overflowLabel = Ui.CreateText(_canvas, string.Empty);
        AnchorToPanel(_overflowLabel, ContentX, ContentY + ContentH + UiTheme.Spacing, ContentW, 22.0f);
        Ui.SetFontSize(_overflowLabel, UiTheme.FontSizeHint);
        Ui.SetTextColor(_overflowLabel, UiTheme.TextMuted);
    }

    public override void OnUpdate(float deltaTime)
    {
        SpawnMenu? spawnMenu = GetScript<SpawnMenu>();
        bool open = spawnMenu is { IsOpen: true };

        if (open != _wasOpen)
        {
            _canvas.SetActive(open);
            if (open)
            {
                _selectedCategory = 0;
                _scrollY = 0.0f;
                Ui.SetTextBoxText(_searchBox, string.Empty);
                _lastSearch = string.Empty;
                _dirty = true;
            }
            _wasOpen = open;
        }

        if (!open)
        {
            return;
        }

        string search = Ui.GetTextBoxText(_searchBox);
        if (search != _lastSearch)
        {
            _lastSearch = search;
            _scrollY = 0.0f;
            _dirty = true;
        }

        for (int i = 0; i < _tabButtons.Count; ++i)
        {
            if (Ui.WasActivated(_tabButtons[i]) && i != _selectedCategory)
            {
                _selectedCategory = i;
                _scrollY = 0.0f;
                _dirty = true;
                break;
            }
        }

        // Scroll only rebuilds offsets (RepositionTiles), never the filtered list itself -
        // no need to route it through the same _dirty flag as a category/search change.
        float scrollDelta = Input.ScrollDelta.Y;
        if (scrollDelta != 0.0f && _lastMaxScroll > 0.0f)
        {
            const float scrollSpeed = 48.0f; // px per wheel notch, tuned to move roughly one tile row at a time
            _scrollY = Math.Clamp(_scrollY - scrollDelta * scrollSpeed, 0.0f, _lastMaxScroll);
            RepositionTiles();
        }

        if (_dirty)
        {
            _dirty = false;
            RefreshFilteredTiles();
        }

        PropSpawner? spawner = GetScript<PropSpawner>();
        if (spawner == null)
        {
            return;
        }
        for (int i = 0; i < _tileCatalogIndex.Count; ++i)
        {
            int catalogIndex = _tileCatalogIndex[i];
            if (catalogIndex < 0)
            {
                continue;
            }
            if (Ui.WasActivated(_tiles[i].Button))
            {
                spawner.SpawnProp(catalogIndex);
                return; // one spawn per click, matching the original panel's own behaviour
            }
        }
    }

    private float _lastMaxScroll;

    /// <summary>Recomputes which catalogue indices match the active category/search,
    /// grows the tile pool to fit (never shrinks it - see the pool's own remarks),
    /// binds each tile's texture/colour/label, and repositions everything for a
    /// scroll position that just reset to 0 (a fresh filter always scrolls to top).</summary>
    private void RefreshFilteredTiles()
    {
        List<int> indices = new();
        bool searching = !string.IsNullOrWhiteSpace(_lastSearch);
        string needle = _lastSearch.Trim();
        for (int i = 0; i < PropSpawner.Catalog.Length; ++i)
        {
            PropDef def = PropSpawner.Catalog[i];
            if (searching)
            {
                // A non-empty search spans the WHOLE catalogue, not just the selected
                // category - the category list is for browsing without a query, exactly
                // Garry's Mod's own split between category tabs and its search box.
                if (def.Name.Contains(needle, StringComparison.OrdinalIgnoreCase))
                {
                    indices.Add(i);
                }
            }
            else if (CategoryOf(def.Name) == _categories[_selectedCategory])
            {
                indices.Add(i);
            }
        }

        int shown = Math.Min(indices.Count, MaxRenderedTiles);
        while (_tiles.Count < shown)
        {
            _tiles.Add(CreateTile());
            _tileCatalogIndex.Add(-1);
        }

        for (int i = 0; i < _tiles.Count; ++i)
        {
            if (i >= shown)
            {
                _tileCatalogIndex[i] = -1;
                _tiles[i].Button.SetActive(false);
                _tiles[i].Swatch.SetActive(false);
                _tiles[i].Label.SetActive(false);
                continue;
            }

            int catalogIndex = indices[i];
            PropDef def = PropSpawner.Catalog[catalogIndex];
            _tileCatalogIndex[i] = catalogIndex;
            _tiles[i].Button.SetActive(true);
            _tiles[i].Swatch.SetActive(true);
            _tiles[i].Label.SetActive(true);

            if (!string.IsNullOrEmpty(def.IconPath))
            {
                // Pooled tiles are reused across categories/searches; a tile that
                // previously showed a fallback swatch left a real colour tint on this
                // image (SetImageColor below), which otherwise keeps multiplying every
                // icon texture shown here afterward - confirmed live: reused tiles
                // rendered icons crushed near-black under a leftover tan/orange
                // primitive tint. Must reset to neutral white whenever an icon is shown.
                Ui.SetImageColor(_tiles[i].Swatch, Vector4.One);
                Ui.SetImageTexture(_tiles[i].Swatch, def.IconPath);
            }
            else
            {
                // No baked icon yet - fall back to the flat colour swatch, never a blank
                // or broken-texture tile, and always keep the name/mass label legible:
                // that fallback tile IS the fix for "the grid looks mostly empty".
                Ui.SetImageTexture(_tiles[i].Swatch, string.Empty);
                Ui.SetImageColor(_tiles[i].Swatch, new Vector4(def.Color, 1.0f));
            }
            Ui.SetText(_tiles[i].Label, $"{def.Name}\n{def.Mass:0.#} kg");
        }

        Ui.SetText(_overflowLabel,
            indices.Count > MaxRenderedTiles
                ? $"{indices.Count} matches - showing first {MaxRenderedTiles}, refine your search"
                : $"{indices.Count} {(indices.Count == 1 ? "match" : "matches")}");

        int rows = (shown + Columns - 1) / Columns;
        float contentHeight = rows * (TileH + TileGap);
        _lastMaxScroll = Math.Max(0.0f, contentHeight - ContentH);
        _scrollY = Math.Clamp(_scrollY, 0.0f, _lastMaxScroll);
        RepositionTiles();
    }

    /// <summary>Applies the current scroll offset to every active tile's rect. Split out
    /// from <see cref="RefreshFilteredTiles"/> so a plain scroll-wheel tick (no filter
    /// change) is a cheap reposition, not a full rebind of every tile's texture/label.</summary>
    private void RepositionTiles()
    {
        for (int i = 0; i < _tileCatalogIndex.Count; ++i)
        {
            if (_tileCatalogIndex[i] < 0)
            {
                continue;
            }
            int col = i % Columns;
            int row = i / Columns;
            // Local to _gridClip (see CreateTile's SetParent) - NOT AnchorToPanel's
            // screen-centre space, since these are the "content child" SetClip's own
            // docs describe: a real ECS child of the clip element, scrolled by moving
            // its rect in the clip's own local space, not the panel's.
            float x = col * (TileW + TileGap);
            float y = row * (TileH + TileGap) - _scrollY;

            Tile tile = _tiles[i];
            Ui.SetRect(tile.Button, x, y, TileW, TileH);
            Ui.SetRect(tile.Swatch, x + 10.0f, y + 10.0f, TileW - 20.0f, TileH - 52.0f);
            Ui.SetRect(tile.Label, x, y + TileH - 40.0f, TileW, 36.0f);
        }
    }

    private Tile CreateTile()
    {
        Entity button = Ui.CreateButton(_canvas);
        Entity swatch = Ui.CreateImage(_canvas);
        Entity label = Ui.CreateText(_canvas, string.Empty);

        // Real ECS children of the clip element - SetClip masks "its whole subtree"
        // (see BuildGridClip's own comment), which requires an actual parent edge, not
        // just visually-overlapping siblings. Anchored (0,0)-(0,0) pivot (0,0) so each
        // tile's SetRect below is plain local top-left-relative pixels within the clip,
        // exactly the "content child positioned via SetOffsets" pattern Ui.SetClip's own
        // doc comment describes.
        foreach (Entity e in stackalloc[] { button, swatch, label })
        {
            e.SetParent(_gridClip);
            Ui.SetAnchors(e, Vector2.Zero, Vector2.Zero);
            Ui.SetPivot(e, Vector2.Zero);
        }

        Ui.SetButtonLabel(button, string.Empty);
        Ui.SetImageCornerRadius(swatch, 4.0f);
        Ui.SetTextAlign(label, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetFontSize(label, UiTheme.FontSizeHint);
        Ui.SetTextColor(label, UiTheme.TextColor);
        Ui.SetTextWrap(label, true);

        return new Tile(button, swatch, label);
    }

    private void Close()
    {
        _canvas.SetActive(false);
    }

    private static string CategoryOf(string propName) => KnownCategories.TryGetValue(propName, out string? category) ? category : FallbackCategory;
}

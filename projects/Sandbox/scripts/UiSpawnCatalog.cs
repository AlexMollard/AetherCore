using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The spawn menu's actual panel content: category tabs across the top and a fixed-size,
/// paged grid of tiles below - the replacement for <c>SpawnMenu</c>'s old one-row-per-prop
/// flat list, which sized itself as <c>100 + Catalog.Length * 48</c>px and had nowhere to
/// put overflow (<see cref="Ui"/> has no scrollable/clipping container at all). Adding a
/// prop from here on is purely a new <see cref="PropSpawner.Catalog"/> row; this file's own
/// construction code (tabs + a grid that never grows) does not change again.
/// </summary>
/// <remarks>
/// <para>
/// <b>Sibling of <c>SpawnMenu</c>, not its replacement.</b> <c>SpawnMenu</c> keeps owning
/// the state machine it always has - <see cref="SpawnMenu.IsOpen"/>, the Q-key toggle, the
/// authority gate - this only reads that public getter to know when to show, and calls
/// <see cref="PropSpawner.SpawnProp"/> directly on a tile click, the exact call
/// <c>SpawnMenu</c>'s own (now removed) button handler used to make, preserving
/// <c>PropSpawner</c>'s "exactly one way a prop comes into the world" invariant. Requires
/// two small coordinated changes elsewhere - see the routed diff notes: <c>SpawnMenu.cs</c>'s
/// old panel-building internals need to be gutted (so its flat list does not render
/// alongside this one), and <c>NetPlayerRig.cs</c> needs one more
/// <c>camera.AddScript(nameof(UiSpawnCatalog))</c> alongside its existing three, since this
/// has to live on the same per-connection camera entity <c>SpawnMenu</c>/<c>PropSpawner</c>
/// already do to reach them with a bare <see cref="EntityScript.GetScript{T}"/>.
/// </para>
/// <para>
/// <b>Thumbnails are flat colour swatches, not icons.</b> Every catalogue entry already
/// carries its authored <see cref="PropDef.Color"/> (the exact colour the prop spawns
/// with); a swatch reuses it directly. A live-rendered physics thumbnail would have been
/// actively misleading while the Collider-rebuild bug was open - it would have shown the
/// wrong collision shape - so this deliberately never renders one. Real per-prop icon
/// textures are a follow-up asset-pipeline deliverable; swapping one in later is a single
/// <see cref="Ui.SetImageTexture"/> call per tile, left as a comment below rather than
/// pointed at a texture path that does not exist yet.
/// </para>
/// <para>
/// <b>Categories are a local lookup, not a new <see cref="PropDef"/> field.</b>
/// <c>PropSpawner.Catalog</c> is being actively extended elsewhere in this project right
/// now (see <c>PropCatalogExtension.md</c>); adding a field to the shared
/// <see cref="PropDef"/> struct risks colliding with that concurrent edit. This keeps its
/// own name-keyed table instead, pre-populated with both today's six primitive names and
/// the ten real-model names that document proposes, so the grid categorises them correctly
/// the moment they land with zero further changes here - anything not recognised falls
/// back to "Misc" rather than being silently dropped.
/// </para>
/// </remarks>
public sealed class UiSpawnCatalog : EntityScript
{
    // Preferred tab order; a tab is only created for a category that at least one
    // CURRENT Catalog entry actually maps to, so this never shows an empty category.
    private static readonly string[] CategoryOrder = { "Crates", "Balls", "Furniture", "Containers", "Misc" };

    private const string FallbackCategory = "Misc";

    // Name -> category. Covers today's six primitives AND the ten real-model names
    // PropCatalogExtension.md proposes handing to PropSpawner.cs's owner, so this needs no
    // update the moment that catalogue lands - only a genuinely new, unanticipated prop
    // name would fall through to FallbackCategory.
    private static readonly Dictionary<string, string> KnownCategories = new()
    {
        ["Small Crate"] = "Crates",
        ["Medium Crate"] = "Crates",
        ["Heavy Crate"] = "Crates",
        ["Large Crate"] = "Crates",
        ["Long Crate"] = "Crates",
        ["Marble"] = "Balls",
        ["Ball"] = "Balls",
        ["Heavy Ball"] = "Balls",
        ["Chair"] = "Furniture",
        ["Table"] = "Furniture",
        ["Bench"] = "Furniture",
        ["Barrel"] = "Containers",
        ["Trash Can"] = "Containers",
        ["Cardboard Box"] = "Containers",
        ["Traffic Cone"] = "Misc",
    };

    private const int Columns = 4;
    private const int Rows = 2;
    private const int PageSize = Columns * Rows;

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
    private Entity _panel;
    private readonly List<Entity> _tabButtons = new();
    private readonly List<string> _categories = new();
    private readonly Tile[] _tiles = new Tile[PageSize];
    private Entity _prevPageButton;
    private Entity _nextPageButton;
    private Entity _pageLabel;

    // catalogIndex per tile slot on the CURRENT page; -1 = slot unused this page.
    private readonly int[] _tileCatalogIndex = new int[PageSize];

    private int _selectedCategory;
    private int _page;

    private bool _wasOpen;

    public override void OnAttach()
    {
        _canvas = Ui.CreateCanvas();
        _canvas.MarkTransient(); // runtime UI, never save-worthy - see PhysicsGun.EnsureHud's own comment on why

        _panel = Ui.CreateImage(_canvas);
        Ui.SetAnchors(_panel, Vector2.Zero, Vector2.Zero);
        Ui.SetPivot(_panel, Vector2.Zero);
        Ui.SetRect(_panel, 40.0f, 40.0f, 560.0f, 420.0f);
        Ui.SetImageColor(_panel, UiTheme.PanelBackground);
        Ui.SetImageCornerRadius(_panel, UiTheme.PanelCornerRadius);

        Entity title = Ui.CreateText(_canvas, "Spawn Menu  (Q to close)");
        Ui.SetAnchors(title, Vector2.Zero, Vector2.Zero);
        Ui.SetPivot(title, Vector2.Zero);
        Ui.SetRect(title, 60.0f, 55.0f, 520.0f, 28.0f);
        Ui.SetFontSize(title, UiTheme.FontSizeBody);
        Ui.SetTextColor(title, UiTheme.TextColor);

        BuildCategoryTabs();
        BuildGrid();
        BuildPager();

        Close();
    }

    /// <summary>One tab button per category that at least one current catalogue entry
    /// actually maps to, in <see cref="CategoryOrder"/>'s preferred order. Built once from
    /// today's <see cref="PropSpawner.Catalog"/> - a category is rare enough (five, total)
    /// that adding one is worth touching this method; adding a PROP never is.</summary>
    private void BuildCategoryTabs()
    {
        HashSet<string> present = new();
        foreach (PropDef def in PropSpawner.Catalog)
        {
            present.Add(CategoryOf(def.Name));
        }
        foreach (string category in CategoryOrder)
        {
            if (!present.Contains(category))
            {
                continue;
            }
            _categories.Add(category);
        }
        // Nothing recognised at all (an empty or fully-unknown catalogue) still needs one
        // tab to select, so the grid has a category to filter by.
        if (_categories.Count == 0)
        {
            _categories.Add(FallbackCategory);
        }

        for (int i = 0; i < _categories.Count; ++i)
        {
            Entity tab = Ui.CreateButton(_canvas);
            Ui.SetAnchors(tab, Vector2.Zero, Vector2.Zero);
            Ui.SetPivot(tab, Vector2.Zero);
            Ui.SetRect(tab, 60.0f + i * 110.0f, 95.0f, 100.0f, 32.0f);
            Ui.SetButtonLabel(tab, _categories[i]);
            _tabButtons.Add(tab);
        }
    }

    private void BuildGrid()
    {
        const float gridX = 60.0f;
        const float gridY = 140.0f;
        const float tileW = 120.0f;
        const float tileH = 110.0f;
        const float gap = 10.0f;

        for (int row = 0; row < Rows; ++row)
        {
            for (int col = 0; col < Columns; ++col)
            {
                int slot = row * Columns + col;
                float x = gridX + col * (tileW + gap);
                float y = gridY + row * (tileH + gap);

                Entity button = Ui.CreateButton(_canvas);
                Ui.SetAnchors(button, Vector2.Zero, Vector2.Zero);
                Ui.SetPivot(button, Vector2.Zero);
                Ui.SetRect(button, x, y, tileW, tileH);
                Ui.SetButtonLabel(button, string.Empty);

                Entity swatch = Ui.CreateImage(_canvas);
                Ui.SetAnchors(swatch, Vector2.Zero, Vector2.Zero);
                Ui.SetPivot(swatch, Vector2.Zero);
                Ui.SetRect(swatch, x + 8.0f, y + 8.0f, tileW - 16.0f, tileH - 40.0f);
                Ui.SetImageCornerRadius(swatch, 4.0f);
                // Texture vs. colour fill is decided per-refresh in RefreshPage, keyed off
                // PropDef.IconPath - see that method's own comment.

                Entity label = Ui.CreateText(_canvas, string.Empty);
                Ui.SetAnchors(label, Vector2.Zero, Vector2.Zero);
                Ui.SetPivot(label, Vector2.Zero);
                Ui.SetRect(label, x, y + tileH - 28.0f, tileW, 24.0f);
                Ui.SetTextAlign(label, UiHAlign.Center, UiVAlign.Middle);
                Ui.SetFontSize(label, UiTheme.FontSizeHint);
                Ui.SetTextColor(label, UiTheme.TextColor);

                _tiles[slot] = new Tile(button, swatch, label);
                _tileCatalogIndex[slot] = -1;
            }
        }
    }

    private void BuildPager()
    {
        const float y = 140.0f + Rows * (110.0f + 10.0f) + 10.0f;

        _prevPageButton = Ui.CreateButton(_canvas);
        Ui.SetAnchors(_prevPageButton, Vector2.Zero, Vector2.Zero);
        Ui.SetPivot(_prevPageButton, Vector2.Zero);
        Ui.SetRect(_prevPageButton, 60.0f, y, 80.0f, 28.0f);
        Ui.SetButtonLabel(_prevPageButton, "< Prev");

        _pageLabel = Ui.CreateText(_canvas, string.Empty);
        Ui.SetAnchors(_pageLabel, Vector2.Zero, Vector2.Zero);
        Ui.SetPivot(_pageLabel, Vector2.Zero);
        Ui.SetRect(_pageLabel, 150.0f, y, 340.0f, 28.0f);
        Ui.SetTextAlign(_pageLabel, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetFontSize(_pageLabel, UiTheme.FontSizeHint);
        Ui.SetTextColor(_pageLabel, UiTheme.TextMuted);

        _nextPageButton = Ui.CreateButton(_canvas);
        Ui.SetAnchors(_nextPageButton, Vector2.Zero, Vector2.Zero);
        Ui.SetPivot(_nextPageButton, Vector2.Zero);
        Ui.SetRect(_nextPageButton, 480.0f, y, 80.0f, 28.0f);
        Ui.SetButtonLabel(_nextPageButton, "Next >");
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
                RefreshPage();
            }
            _wasOpen = open;
        }

        if (!open)
        {
            return;
        }

        for (int i = 0; i < _tabButtons.Count; ++i)
        {
            if (Ui.WasActivated(_tabButtons[i]) && i != _selectedCategory)
            {
                _selectedCategory = i;
                _page = 0;
                RefreshPage();
                return;
            }
        }

        if (Ui.WasActivated(_prevPageButton))
        {
            _page = System.Math.Max(0, _page - 1);
            RefreshPage();
            return;
        }
        if (Ui.WasActivated(_nextPageButton))
        {
            _page += 1;
            RefreshPage();
            return;
        }

        PropSpawner? spawner = GetScript<PropSpawner>();
        if (spawner == null)
        {
            return;
        }
        for (int slot = 0; slot < PageSize; ++slot)
        {
            int catalogIndex = _tileCatalogIndex[slot];
            if (catalogIndex < 0)
            {
                continue;
            }
            if (Ui.WasActivated(_tiles[slot].Button))
            {
                spawner.SpawnProp(catalogIndex);
                return; // one spawn per click, matching SpawnMenu's own old behaviour
            }
        }
    }

    /// <summary>Redraws the grid/pager for <see cref="_selectedCategory"/>/<see cref="_page"/>
    /// from <see cref="PropSpawner.Catalog"/> as it exists RIGHT NOW - never cached, so a
    /// catalogue that grows between menu opens (e.g. a hot-reloaded script assembly) is
    /// picked up on the very next Q press with no extra work.</summary>
    private void RefreshPage()
    {
        string category = _categories.Count > 0 ? _categories[_selectedCategory] : FallbackCategory;

        List<int> indices = new();
        for (int i = 0; i < PropSpawner.Catalog.Length; ++i)
        {
            if (CategoryOf(PropSpawner.Catalog[i].Name) == category)
            {
                indices.Add(i);
            }
        }

        int pageCount = System.Math.Max(1, (indices.Count + PageSize - 1) / PageSize);
        _page = System.Math.Clamp(_page, 0, pageCount - 1);

        for (int slot = 0; slot < PageSize; ++slot)
        {
            int listIndex = _page * PageSize + slot;
            if (listIndex >= indices.Count)
            {
                _tileCatalogIndex[slot] = -1;
                _tiles[slot].Button.SetActive(false);
                _tiles[slot].Swatch.SetActive(false);
                _tiles[slot].Label.SetActive(false);
                continue;
            }

            int catalogIndex = indices[listIndex];
            PropDef def = PropSpawner.Catalog[catalogIndex];
            _tileCatalogIndex[slot] = catalogIndex;
            _tiles[slot].Button.SetActive(true);
            _tiles[slot].Swatch.SetActive(true);
            _tiles[slot].Label.SetActive(true);
            if (!string.IsNullOrEmpty(def.IconPath))
            {
                // Baked icon drives the tile; the swatch colour underneath never shows.
                Ui.SetImageTexture(_tiles[slot].Swatch, def.IconPath);
            }
            else
            {
                // No baked icon yet - fall back to the flat colour swatch, never a blank
                // or broken-texture tile. Clearing the texture is required, not optional:
                // tile slots are recycled across pages/categories, so a slot that just
                // showed CrateSmall's icon must not keep showing it under Bench's label.
                Ui.SetImageTexture(_tiles[slot].Swatch, string.Empty);
                Ui.SetImageColor(_tiles[slot].Swatch, new Vector4(def.Color, 1.0f));
            }
            Ui.SetText(_tiles[slot].Label, $"{def.Name}\n{def.Mass:0.#} kg");
        }

        Ui.SetText(_pageLabel, $"{category}  -  page {_page + 1}/{pageCount}");
        _prevPageButton.SetActive(pageCount > 1);
        _nextPageButton.SetActive(pageCount > 1);
    }

    private void Close()
    {
        _canvas.SetActive(false);
    }

    private static string CategoryOf(string propName) => KnownCategories.TryGetValue(propName, out string? category) ? category : FallbackCategory;
}

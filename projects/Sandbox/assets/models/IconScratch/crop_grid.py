"""
Crops the 10 modelled-prop icons from a File Explorer grid-view screenshot,
calibrated empirically against the live Editor rather than assumed.

Grid formula (Props folder, "100 px" tile size, this session's screenshot geometry):
  col_x(i) = 446 + i*114
  row_y(j) = 781 + j*135
  icon crop = (x, y, x+114, y+104)  # 104-tall excludes the label text row

File order in the Props folder grid is alphabetical, FILES ONLY (folders render in
the separate tree pane, not this grid): Barrel.glb(0), Barrel.mesh(1), Bench.glb(2),
Bench.mesh(3), CardboardBox.glb(4), CardboardBox.mesh(5), Chair.glb(6), Chair.mesh(7),
Cone.glb(8), Cone.mesh(9), CrateLarge.glb(10), CrateLarge.mesh(11), CrateLong.glb(12),
CrateLong.mesh(13), CrateSmall.glb(14), CrateSmall.mesh(15), Table.glb(16),
Table.mesh(17), TrashCan.glb(18), TrashCan.mesh(19) - 7 columns per row.
"""
import sys

from PIL import Image

COL_X0 = 446
ROW_Y0 = 781
COL_SPACING = 114
ROW_SPACING = 135
ICON_W = 114
ICON_H = 104
COLS_PER_ROW = 7

# name -> flat index in the alphabetical files-only listing above
INDEX = {
    "Barrel": 0,
    "Bench": 2,
    "CardboardBox": 4,
    "Chair": 6,
    "Cone": 8,
    "CrateLarge": 10,
    "CrateLong": 12,
    "CrateSmall": 14,
    "Table": 16,
    "TrashCan": 18,
}


def tile_rect(index):
    col = index % COLS_PER_ROW
    row = index // COLS_PER_ROW
    x = COL_X0 + col * COL_SPACING
    y = ROW_Y0 + row * ROW_SPACING
    return (x, y, x + ICON_W, y + ICON_H)


def main():
    screenshot_path, out_dir = sys.argv[1], sys.argv[2]
    shot = Image.open(screenshot_path)
    for name, index in INDEX.items():
        rect = tile_rect(index)
        crop = shot.crop(rect)
        out_path = f"{out_dir}/{name}_raw.png"
        crop.save(out_path)
        print(f"{name}: index {index} rect {rect} -> {out_path}")


if __name__ == "__main__":
    main()

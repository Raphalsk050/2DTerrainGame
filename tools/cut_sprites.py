"""Cuts one row out of a top-down sprite pack into a side-on animation strip.

The packs these come from are drawn for a top-down game: four rows of the same
animation, one per facing — away, three-quarter, and side. Only the side row is any
use in a platformer, and it is the last one.

Run it again whenever a pack is added or replaced; it is not a one-off. The whole
point of it being a file rather than something typed once into a shell is that the
crop window is a *decision* — see `kWindow` — and a decision that is not written down
somewhere gets made differently the second time.

    python tools/cut_sprites.py

What it writes is a horizontal strip per animation, one frame per cell, trimmed to a
window shared by every animation of that creature. Shared, and that is the important
part: the frames have to agree about where the ground is and where the middle of the
body is, or a boar changes height and jumps sideways the moment it starts walking.

Nothing is mirrored here. The game mirrors at draw time from one right-facing strip,
which is what `figure::Draw` already does for the hand-drawn art — a baked left-facing
copy is twice the asset and a second thing to keep in step.
"""

import os
from PIL import Image

# Where the packs live and where the game keeps its art.
HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)

PACK = os.path.join(
    os.path.expanduser("~"),
    "Downloads",
    "craftpix-net-789196-free-top-down-hunt-animals-pixel-sprite-pack",
    "PNG",
    "Without_shadow",
)

# One cell of the source sheet.
CELL = 32

# Which row of the four holds the side view. The last, in every pack seen so far.
SIDE_ROW = 3

# The window cut out of each cell, as (left, top, right, bottom) inside the cell.
#
# Measured from the art rather than guessed: across every frame of idle, walk and run
# the boar occupies x 1..29 and y 5..26, and **the feet are on y = 26 in all of them**.
# So the bottom of the window is the ground line, which is what lets the game anchor a
# frame by its feet and never have the creature sink or float between animations.
#
# One window for every animation of a creature, for the same reason.
WINDOW = (1, 5, 29, 26)

# Each creature: where its sheets are, and what to call the strips.
CREATURES = {
    "boar": {
        "folder": "Boar",
        "clips": {"idle": "Boar_Idle", "walk": "Boar_Walk", "run": "Boar_Run"},
    },
}


def cut(sheet_path, out_path):
    sheet = Image.open(sheet_path).convert("RGBA")

    cols = sheet.width // CELL

    left, top, right, bottom = WINDOW

    wide = right - left
    tall = bottom - top

    strip = Image.new("RGBA", (wide * cols, tall), (0, 0, 0, 0))

    for c in range(cols):
        frame = sheet.crop(
            (
                c * CELL + left,
                SIDE_ROW * CELL + top,
                c * CELL + right,
                SIDE_ROW * CELL + bottom,
            )
        )

        strip.paste(frame, (c * wide, 0))

    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    strip.save(out_path)

    return cols, wide, tall


def main():
    for name, what in CREATURES.items():
        folder = os.path.join(PACK, what["folder"])

        for clip, sheet in what["clips"].items():
            src = os.path.join(folder, sheet + ".png")
            dst = os.path.join(GAME, "assets", "mobs", name, clip + ".png")

            cols, wide, tall = cut(src, dst)

            print("%-6s %-5s %d frames of %dx%d -> assets/mobs/%s/%s.png" % (name, clip, cols, wide, tall, name, clip))


if __name__ == "__main__":
    main()

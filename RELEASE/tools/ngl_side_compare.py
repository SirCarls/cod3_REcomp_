#!/usr/bin/env python3
"""Compare Call of Duty 3's own pictures with the native renderer's, from a run with cod3_debug_ngl_side on.

With cod3_debug_ngl_side=true the game saves, every 10 s for its first 80 s (frames 600 to 4800), the frame drawn
natively (ngl-side-<frame>-vk.ppm) and asks for its own screenshot of the same moment (a .png); the log names both on
one line. For each frame asked for, this tool finds that line, puts the two pictures side by side (the game's on the
left, the native one on the right, at half size) and says how far apart they are:

  - diff: the mean difference per color channel over the whole picture (0 the same, 255 opposite);
  - worst: the same for the worst of a 4 x 4 grid of regions, and where it is (row, column, from the top left);
  - brightness: each picture's mean, to tell "too dark" from "wrong".

--grid also prints, for each region of the 4 x 4 grid, the game's brightness and the native one's, and each
picture's mean red, green and blue: a region much darker natively points at something not drawn there, the whole
grid darker at the light.

The native picture compared is the whole one (3D, then the HUD and menus' 2D over it), since the game's screenshot
has both; --3d compares the native 3D alone (ngl-side-<frame>-3d.ppm), when it was saved.

Compare right after the run: the native pictures keep their names from run to run, so the next run with the side
check on overwrites them (the game's screenshots are named by the time). The tool warns when the two pictures were
saved far apart.

Usage:
  ngl_side_compare.py LOG FRAME [FRAME ...] [--out FOLDER] [--3d] [--grid]

  LOG     the game's log file of that run (its --log_file, or userdata/logs/ in the install)
  FRAME   frame numbers: 600, 1200, ... 4800
  --out   where the side-by-side pictures go (cmp<frame>.png); default: next to the log

Needs Pillow (pip install pillow). Paths may be Windows or Linux ones.
"""
import argparse
import os
import re
import sys

from PIL import Image, ImageChops, ImageStat

GRID = 4


def mean(picture):
    return sum(ImageStat.Stat(picture).mean) / 3


def worst_region(diff):
    cell_w, cell_h = diff.width // GRID, diff.height // GRID
    cells = [(mean(diff.crop((c * cell_w, r * cell_h, (c + 1) * cell_w, (r + 1) * cell_h))), r, c)
             for r in range(GRID) for c in range(GRID)]
    return max(cells)


def print_grid(game, native):
    """Each region's brightness, the game's / the native one's, row by row; then each picture's mean color."""
    cell_w, cell_h = game.width // GRID, game.height // GRID
    for r in range(GRID):
        cells = []
        for c in range(GRID):
            box = (c * cell_w, r * cell_h, (c + 1) * cell_w, (r + 1) * cell_h)
            cells.append('%3.0f/%3.0f' % (mean(game.crop(box)), mean(native.crop(box))))
        print('    row %d: %s' % (r, '  '.join(cells)))
    print('    color: game %s, native %s' % tuple(
        ' '.join('%.0f' % v for v in ImageStat.Stat(p).mean) for p in (game, native)))


def find_pictures(log, frame):
    """The native picture's path and the game's screenshot's, from the log line of this frame, or None."""
    line = re.search(r"frame %d: [^\n]*? to (.+?-%d-vk\.ppm); the game's own: (.+?\.png)" % (frame, frame), log)
    if not line:
        return None
    native = line.group(1)
    folder = os.path.dirname(native)
    return native, os.path.join(folder, 'ngl-side-%d-3d.ppm' % frame), os.path.join(folder, line.group(2))


def compare(log, frame, out, only_3d, grid):
    found = find_pictures(log, frame)
    if not found:
        print(frame, 'no picture line in the log')
        return
    whole, alone_3d, game = found
    choices = [alone_3d, whole] if only_3d else [whole, alone_3d]
    native = next((p for p in choices if os.path.exists(p)), None)
    if not native or not os.path.exists(game):
        print(frame, 'missing:', native or whole, game)
        return
    # the native pictures' names don't change from run to run, so a later run in the same install overwrites them
    apart = abs(os.path.getmtime(native) - os.path.getmtime(game))
    if apart > 30:
        print('%d: warning: the two pictures were saved %.0f s apart; the native one may be from a later run'
              % (frame, apart))
    a = Image.open(game).convert('RGB')
    b = Image.open(native).convert('RGB').resize(a.size)
    both = Image.new('RGB', (a.width * 2, a.height))
    both.paste(a, (0, 0))
    both.paste(b, (a.width, 0))
    both = both.resize((both.width // 2, both.height // 2))
    both.save(os.path.join(out, 'cmp%d.png' % frame))
    diff = ImageChops.difference(a, b)
    worst, row, column = worst_region(diff)
    print('%d: %s vs %s: diff %.1f, worst %.1f at row %d column %d, brightness game %.0f native %.0f'
          % (frame, os.path.basename(native), os.path.basename(game), mean(diff), worst, row, column, mean(a), mean(b)))
    if grid:
        print_grid(a, b)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('log', help="the run's log file")
    parser.add_argument('frames', nargs='+', type=int, help='frame numbers (600, 1200, ... 4800)')
    parser.add_argument('--out', help='folder for the side-by-side pictures (default: next to the log)')
    parser.add_argument('--3d', dest='only_3d', action='store_true', help='compare the native 3D alone')
    parser.add_argument('--grid', action='store_true', help="print each region's brightness and the mean colors")
    args = parser.parse_args()
    with open(args.log, encoding='utf-8', errors='replace') as f:
        log = f.read()
    out = args.out or os.path.dirname(os.path.abspath(args.log))
    os.makedirs(out, exist_ok=True)
    for frame in args.frames:
        compare(log, frame, out, args.only_3d, args.grid)
    return 0


if __name__ == '__main__':
    sys.exit(main())

"""Compare Windows menu header positions with 1920x1080 Global Android captures."""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image


# Coordinates enclose stationary controls in the original English 16:9 HUD.
SUMMONS = {
    'gem icon': (683, 53, 716, 91),
    'gem balance': (795, 61, 906, 81),
    'gem plus': (913, 54, 950, 89),
    'Bunker': (1006, 55, 1043, 87),
    'Notices': (1108, 54, 1143, 87),
}
TABS = ('Gacha', 'Organization', 'Enhance', 'Quest', 'Map', 'Library', 'Menu')


def load_frame(prefix, tab, android=False):
    path = prefix.with_name(f'{prefix.name}-{tab}.png')
    with Image.open(path) as image:
        if android and image.size != (1920, 1080):
            raise ValueError(f'{path} must be a native 1920x1080 Android reference')
        if image.width * 9 != image.height * 16:
            raise ValueError(f'{path} must use a 16:9 game viewport')
        return np.asarray(image.convert('L').resize((1920, 1080), Image.Resampling.BOX),
                          dtype=np.float32)


def horizontal_offset(reference, capture, box):
    left, top, right, bottom = box
    edges = np.diff(reference[top:bottom, left:right], axis=1)
    norm = np.linalg.norm(edges)
    if not norm:
        raise ValueError(f'No reference edges in {box}')
    candidates = []
    for offset in range(-40, 41):
        window = capture[top:bottom, left + offset:right + offset]
        sample = np.diff(window, axis=1)
        length = np.linalg.norm(sample)
        score = np.sum(edges * sample) / (norm * length) if length else 0.0
        candidates.append((score, -abs(offset), offset))
    score, _, offset = max(candidates)
    return offset, score


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('android', type=Path, help='reference filename prefix, e.g. captures/android')
    parser.add_argument('windows', type=Path, help='Windows filename prefix, e.g. captures/3840x2160')
    args = parser.parse_args()
    passed = True
    for tab in TABS:
        reference = load_frame(args.android, tab, android=True)
        capture = load_frame(args.windows, tab)
        controls = SUMMONS if tab == 'Gacha' else {'counter divider': (964, 48, 975, 86)}
        for name, box in controls.items():
            offset, score = horizontal_offset(reference, capture, box)
            print(f'{tab:14} {name:16} {offset:+3} px   correlation {score:.4f}')
            passed &= offset == 0 and score >= 0.8
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())

"""Compare native and FSR captures from the same stationary field checkpoint.

Run with the packaged Pods Python, passing the native PNG and a candidate PNG.
The regression scaled the scene about the OpenGL viewport origin (bottom left)
while the screen-space HUD remained correctly positioned.
"""
import argparse
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def compare(reference_path, candidate_path):
    with Image.open(reference_path) as reference, Image.open(candidate_path) as candidate:
        if reference.width * candidate.height != candidate.width * reference.height:
            raise ValueError('Capture aspect ratios differ')
        width = 480
        height = round(width * reference.height / reference.width)
        native = reference.convert('L').resize((width, height), Image.Resampling.LANCZOS)
        actual = candidate.convert('L').resize((width, height), Image.Resampling.LANCZOS)
    # Exclude the header, joystick and right-hand menu. Only scenery votes.
    region = (width // 12, height // 5, width * 5 // 6, height * 4 // 5)
    actual = actual.crop(region)
    if ImageStat.Stat(actual).stddev[0] < 10:
        raise ValueError('A loaded, textured 3D scene is required')

    def difference(scale):
        box = (0, height - height / scale, width / scale, height)
        projected = native.resize(native.size, Image.Resampling.BICUBIC, box)
        return ImageStat.Stat(ImageChops.difference(projected.crop(region), actual)).mean[0]

    scales = [n / 100 for n in range(100, 301)]
    best = min(scales, key=difference)
    return best, difference(1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('native', type=Path)
    parser.add_argument('candidate', type=Path)
    args = parser.parse_args()
    scale, error = compare(args.native, args.candidate)
    print(f'Best matching native crop {scale:.2f}x. Full-frame mean luma error {error:.1f}/255.')
    if scale > 1.03 or error > 20:
        raise SystemExit('Framing differs or the captures show different camera positions.')

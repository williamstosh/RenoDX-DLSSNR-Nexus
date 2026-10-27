"""Rasterize the test fixture's real ImGui mesh/atlas for visual inspection."""
import argparse
import json
import math
import sys
from functools import lru_cache
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


@lru_cache(maxsize=1)
def load_atlas(path, width, height, modified_ns):
    # A run's meshes share an immutable atlas. Keep only the current one in RAM.
    return np.frombuffer(path.read_bytes(), dtype=np.uint8).reshape(height, width, 4)


def render(path, output_directory=None, image_format='png'):
    mesh = json.loads(path.read_text(encoding='utf-8'))
    width, height = int(mesh['width']), int(mesh['height'])
    atlas_path = capture_atlas(path, mesh)
    atlas = load_atlas(atlas_path, mesh['atlas_width'], mesh['atlas_height'], atlas_path.stat().st_mtime_ns)
    image = np.zeros((height, width, 3), np.float32)
    image[:] = (0.055, 0.06, 0.07)
    for draw in mesh['lists']:
        vertices = np.array(draw['vertices'], dtype=np.float64)
        indices = np.array(draw['indices'], dtype=np.int32)
        colors = vertices[:, 4].astype(np.uint32)
        colors = np.stack([(colors >> shift) & 255 for shift in (0, 8, 16, 24)], axis=1) / 255
        for offset, count, base, cx0, cy0, cx1, cy1 in draw['commands']:
            for triangle in indices[offset:offset + count].reshape(-1, 3) + base:
                points = vertices[triangle, :2]
                x0 = max(0, int(np.floor(max(points[:, 0].min(), cx0))))
                y0 = max(0, int(np.floor(max(points[:, 1].min(), cy0))))
                x1 = min(width, int(np.ceil(min(points[:, 0].max(), cx1))))
                y1 = min(height, int(np.ceil(min(points[:, 1].max(), cy1))))
                if x1 <= x0 or y1 <= y0:
                    continue
                a, b, c = points
                area = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
                if abs(area) < 1e-8:
                    continue
                yy, xx = np.mgrid[y0:y1, x0:x1]
                xx, yy = xx + .5, yy + .5
                w0 = ((b[1] - c[1]) * (xx - c[0]) + (c[0] - b[0]) * (yy - c[1])) / area
                w1 = ((c[1] - a[1]) * (xx - c[0]) + (a[0] - c[0]) * (yy - c[1])) / area
                w2 = 1 - w0 - w1
                mask = (w0 >= -1e-7) & (w1 >= -1e-7) & (w2 >= -1e-7)
                if not mask.any():
                    continue
                weights = np.stack((w0[mask], w1[mask], w2[mask]), axis=1)
                uv = weights @ vertices[triangle, 2:4]
                tx = np.clip((uv[:, 0] * mesh['atlas_width']).astype(int), 0, mesh['atlas_width'] - 1)
                ty = np.clip((uv[:, 1] * mesh['atlas_height']).astype(int), 0, mesh['atlas_height'] - 1)
                rgba = (weights @ colors[triangle]) * (atlas[ty, tx].astype(np.float32) / 255)
                target = image[y0:y1, x0:x1]
                target[mask] = rgba[:, :3] * rgba[:, 3:] + target[mask] * (1 - rgba[:, 3:])
    output = (output_directory or path.parent) / (path.stem + '.' + image_format)
    options = {'lossless': True, 'method': 6} if image_format == 'webp' else {}
    Image.fromarray(np.round(image * 255).astype(np.uint8)).save(output, **options)
    print(output)
    return output


def checked_crop(image, bounds):
    """Reject stale semantic bounds rather than silently padding a bad crop."""
    if (not isinstance(bounds, (list, tuple)) or len(bounds) != 4
            or any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) for v in bounds)):
        raise ValueError('Crop requires four finite coordinates')
    x0, y0, x1, y1 = (round(v) for v in bounds)
    if not (0 <= x0 < x1 <= image.width and 0 <= y0 < y1 <= image.height):
        raise ValueError(f'Crop outside capture: {bounds}, image={image.size}')
    return image.crop((x0, y0, x1, y1))


def capture_atlas(path, mesh):
    # Legacy captures use a same-stem atlas; new runs share only within this directory.
    name = mesh.get('atlas_file')
    if name is None:
        return path.with_suffix('.rgba')
    if not isinstance(name, str) or not name or '/' in name or '\\' in name or ':' in name or Path(name).name != name:
        raise ValueError('Capture atlas must be a local filename')
    atlas = path.parent / name
    if atlas.suffix != '.rgba' or atlas.is_symlink() or atlas.resolve().parent != path.parent.resolve():
        raise ValueError('Capture atlas escapes capture directory')
    return atlas


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog='Set NR_NEXUS_DOC_CAPTURES=1 for tools/run-tests.ps1 -Suite ui to create doc-*.json. '
               'Render those with --pattern doc-*.json --documentation --format webp --output-directory <directory>.')
    parser.add_argument('directory', type=Path)
    parser.add_argument('--pattern', default='*.json', help='Select mesh captures, e.g. doc-*.json')
    parser.add_argument('--output-directory', type=Path)
    parser.add_argument('--format', choices=('png', 'webp'), default='png')
    parser.add_argument('--documentation', action='store_true',
                        help='Crop the opt-in doc captures and assemble the four-theme contact sheet')
    args = parser.parse_args()
    if args.documentation and (not args.output_directory or args.format != 'webp'):
        parser.error('--documentation requires --output-directory and --format webp')
    captures = []
    skipped = 0
    for path in sorted(args.directory.glob(args.pattern)):
        try:
            mesh = json.loads(path.read_text(encoding='utf-8'))
            if 'lists' in mesh:
                capture_atlas(path, mesh)
                captures.append(path)
        except (OSError, ValueError, TypeError) as error:
            print(f'Skipping {path.name}: {error}', file=sys.stderr)
            skipped += 1
    if not captures:
        parser.error('No matching mesh/atlas pairs found')
    if args.output_directory:
        args.output_directory.mkdir(parents=True, exist_ok=True)
    for path in captures:
        try:
            render(path, args.output_directory, args.format)
        except (OSError, ValueError, KeyError, TypeError) as error:
            print(f'Skipping {path.name}: {error}', file=sys.stderr)
            skipped += 1
    if args.documentation:
        # Bounds come from the same frame's production control geometry.
        # Old captures without markers must be regenerated after a UI change.
        crops = ('quality', 'multipass', 'results', 'diagnostics', 'advanced')
        themes = (('cyber_neon', 'Cyber Neon'), ('nexus_dark', 'Nexus Dark'),
                  ('fluent_light', 'Fluent Light'), ('midnight_oled', 'Midnight OLED'))
        font = ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf', 24)
        for suffix, language in (('', 1), ('-zh', 14)):
            for name in crops:
                stem = 'doc-' + name + suffix
                mesh = json.loads((args.directory / (stem + '.json')).read_text(encoding='utf-8'))
                if mesh['language'] != language:
                    parser.error('Capture language mismatch: ' + stem)
                with Image.open(args.output_directory / (stem + '.webp')) as original:
                    checked_crop(original, mesh['regions']['doc']).save(
                        args.output_directory / (name + suffix + '.webp'), lossless=True, method=6)
            sheet = Image.new('RGB', (1300, 1220), '#10141b')
            draw = ImageDraw.Draw(sheet)
            for index, (slug, title) in enumerate(themes):
                stem = 'doc-theme-' + slug + suffix
                if json.loads((args.directory / (stem + '.json')).read_text(encoding='utf-8'))['language'] != language:
                    parser.error('Capture language mismatch: ' + stem)
                x, y = index % 2 * 650, index // 2 * 610
                draw.text((x + 20, y + 5), title, font=font, fill='white')
                with Image.open(args.output_directory / (stem + '.webp')) as original:
                    mesh = json.loads((args.directory / (stem + '.json')).read_text(encoding='utf-8'))
                    crop = checked_crop(original, mesh['regions']['doc'])
                    crop.thumbnail((650, 568), Image.Resampling.LANCZOS)
                    sheet.paste(crop, (x, y + 42))
            sheet.save(args.output_directory / ('themes' + suffix + '.webp'), lossless=True, method=6)
    if skipped:
        sys.exit(1)

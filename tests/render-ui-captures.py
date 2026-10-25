"""Rasterize the test fixture's real ImGui mesh/atlas for visual inspection."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def render(path):
    mesh = json.loads(path.read_text(encoding='utf-8'))
    width, height = int(mesh['width']), int(mesh['height'])
    atlas = np.frombuffer(path.with_suffix('.rgba').read_bytes(), dtype=np.uint8)
    atlas = atlas.reshape(mesh['atlas_height'], mesh['atlas_width'], 4).astype(np.float32) / 255
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
                rgba = (weights @ colors[triangle]) * atlas[ty, tx]
                target = image[y0:y1, x0:x1]
                target[mask] = rgba[:, :3] * rgba[:, 3:] + target[mask] * (1 - rgba[:, 3:])
    output = path.with_suffix('.png')
    Image.fromarray(np.round(image * 255).astype(np.uint8)).save(output)
    print(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    for path in args.directory.glob('*.json'):
        if path.with_suffix('.rgba').exists():
            render(path)

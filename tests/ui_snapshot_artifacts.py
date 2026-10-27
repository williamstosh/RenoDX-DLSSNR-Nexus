"""Package production ImGui captures; never promote snapshots or modify existing docs.

Registry entries may specify doc_role and regions.doc. Documentation languages are
1 (English) and 14 (Chinese). All output directories must be new candidates.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import html
import importlib.util
import json
import math
from pathlib import Path, PureWindowsPath
import re
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont


SCHEMA = 'nr-nexus-ui-snapshot/1'
CAVEAT = '真实生产 ImGui 绘制逻辑；合成测试状态，不代表真实游戏运行、GPU 画质或性能验收。'
SAFE_ID = re.compile(r'[A-Za-z0-9][A-Za-z0-9_-]{0,127}\Z')
DOC_ROLES = {'quality', 'multipass', 'results', 'diagnostics', 'advanced', 'presets'}
THEMES = ('cyber_neon', 'nexus_dark', 'fluent_light', 'midnight_oled')
THEME_TITLES = ('Cyber Neon', 'Nexus Dark', 'Fluent Light', 'Midnight OLED')
PAGE_NAMES = {0: '画质', 1: '叠层', 2: '诊断', 3: '高级'}
REQUIRED_DOC_ROLES = {'quality', 'multipass', 'results', 'diagnostics', 'advanced'} | {
    'theme-' + slug for slug in THEMES}


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def safe_file(root, name):
    """Resolve only regular files below root, rejecting Windows and POSIX escapes."""
    if (not isinstance(name, str) or not name or '\\' in name or ':' in name
            or PureWindowsPath(name).is_absolute() or Path(name).is_absolute()
            or any(part in ('', '.', '..') for part in name.split('/'))):
        raise ValueError('unsafe relative file path')
    root = root.resolve()
    path = root / name
    for parent in (path, *path.parents):
        if parent == root:
            break
        if parent.is_symlink() or (hasattr(parent, 'is_junction') and parent.is_junction()):
            raise ValueError('capture path contains a link')
    if not path.resolve().is_relative_to(root) or not path.is_file():
        raise ValueError('capture file missing or outside capture directory')
    return path


def renderer_module():
    spec = importlib.util.spec_from_file_location('ui_snapshot_renderer',
                                                 Path(__file__).with_name('render-ui-captures.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def positive(value, name, integral=False):
    if (isinstance(value, bool) or not isinstance(value, (int, float))
            or not math.isfinite(value) or value <= 0 or (integral and int(value) != value)):
        raise ValueError(f'{name} must be a positive {"integer" if integral else "number"}')


def check_entry(entry, mesh):
    for key in ('page', 'category', 'state', 'theme', 'language', 'font_size',
                'width', 'height', 'kind', 'interaction'):
        if key not in entry:
            raise ValueError(f'missing metadata: {key}')
    for key in ('category', 'state', 'theme', 'kind'):
        if not isinstance(entry[key], str) or not entry[key].strip():
            raise ValueError(f'{key} must be nonempty text')
    if not isinstance(entry['page'], (str, int)) or isinstance(entry['page'], bool):
        raise ValueError('page must be text or integer')
    if not isinstance(entry['interaction'], (str, bool, list, dict)):
        raise ValueError('interaction must describe the captured interaction')
    for key in ('width', 'height', 'language', 'font_size'):
        positive(entry[key], key, integral=key != 'font_size')
    for key in ('width', 'height', 'page', 'theme', 'language', 'font_size'):
        if key not in mesh or mesh[key] != entry[key]:
            raise ValueError(f'mesh/registry metadata mismatch: {key}')
    for key in ('id', 'category', 'state', 'kind', 'interaction', 'checks', 'doc_role'):
        if key in mesh and mesh[key] != entry.get(key):
            raise ValueError(f'mesh/registry metadata mismatch: {key}')
    if not isinstance(mesh.get('lists'), list) or not mesh['lists']:
        raise ValueError('empty ImGui draw lists')
    if 'regions' in entry and entry['regions'] != mesh.get('regions'):
        raise ValueError('mesh/registry metadata mismatch: regions')
    checks = entry.get('checks', {})
    if checks is False or (isinstance(checks, dict) and any(v is False for v in checks.values())):
        raise ValueError('capture reports failed checks')


def check_content(image):
    pixels = np.asarray(image.convert('RGB'))
    if pixels.shape[0] < 2 or pixels.shape[1] < 2 or np.ptp(pixels.reshape(-1, 3), axis=0).max() < 2:
        raise ValueError('image is empty or blank')


def crop_document(image, bounds, renderer):
    if not isinstance(bounds, (list, tuple)) or len(bounds) != 4:
        raise ValueError('documentation requires regions.doc bounds')
    if any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) for v in bounds):
        raise ValueError('documentation bounds must be finite numbers')
    x0, y0, x1, y1 = bounds
    if not (0 <= x0 < x1 <= image.width and 0 <= y0 < y1 <= image.height
            and x1 - x0 >= 2 and y1 - y0 >= 2):
        raise ValueError('documentation bounds outside image or too small')
    # The renderer owns crop rounding and any future mesh-coordinate conventions.
    crop = renderer.checked_crop(image, bounds)
    check_content(crop)
    return crop


def compare_images(candidate, old):
    result = {'candidate_sha256': sha256(candidate), 'old_sha256': None,
              'pixel_identical': False, 'meaningful_change': True,
              'threshold': {'channel_delta': 8, 'changed_fraction': 0.005, 'mean_absolute_error': 0.5}}
    if old is None or not old.is_file():
        return dict(result, comparison='missing')
    result['old_sha256'] = sha256(old)
    with Image.open(candidate) as current, Image.open(old) as previous:
        result.update(candidate_dimensions=list(current.size), old_dimensions=list(previous.size))
        if current.size != previous.size:
            return dict(result, comparison='dimensions_changed')
        delta = np.abs(np.asarray(current.convert('RGBA'), dtype=np.int16)
                       - np.asarray(previous.convert('RGBA'), dtype=np.int16))
    fraction = float(np.mean(np.max(delta, axis=2) > 8))
    mae = float(np.mean(delta))
    return dict(result, comparison='compared', pixel_identical=not bool(delta.any()),
                changed_fraction=fraction, mean_absolute_error=mae,
                meaningful_change=fraction >= 0.005 or mae >= 0.5)


def documentation_candidates(entries, output, existing, renderer):
    output.mkdir(parents=True, exist_ok=False)
    report = {'schema': 'nr-nexus-ui-doc-candidates/1', 'images': [], 'failures': [],
              'caveat': '仅候选；像素阈值辅助筛选，不能证明文字、裁剪或功能说明正确。'}
    targets = Counter()
    for entry in entries:
        role = entry.get('doc_role')
        if role:
            targets[(role, entry['language'])] += 1
    for entry in entries:
        role = entry.get('doc_role')
        if not role:
            continue
        try:
            if role not in DOC_ROLES and role not in {'theme-' + slug for slug in THEMES}:
                raise ValueError('unknown doc_role')
            language = entry['language']
            if language not in (1, 14):
                raise ValueError('documentation language must be 1 or 14')
            if targets[(role, language)] != 1:
                raise ValueError('duplicate documentation role/language')
            name = role + ('-zh' if language == 14 else '') + '.webp'
            with Image.open(entry['_image']) as image:
                crop = crop_document(image, entry.get('regions', {}).get('doc'), renderer)
                crop.save(output / name, lossless=True, method=6)
            old = safe_file(existing, name) if existing and (existing / name).exists() else None
            report['images'].append(dict(id=entry['id'], role=role, language=language, file=name,
                                          **compare_images(output / name, old)))
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
            report['failures'].append({'id': entry['id'], 'reason': str(error)})
    for language in (1, 14):
        suffix = '-zh' if language == 14 else ''
        names = ['theme-' + slug + suffix + '.webp' for slug in THEMES]
        if not all(any(item['file'] == name for item in report['images']) for name in names):
            continue
        tiles = []
        for name in names:
            with Image.open(output / name) as image:
                tiles.append(image.convert('RGB'))
        # Native pixel sizes, no resizing: each row/column accommodates its largest tile.
        widths = [max(tiles[i].width for i in (col, col + 2)) for col in (0, 1)]
        heights = [max(tile.height for tile in tiles[row:row + 2]) + 40 for row in (0, 2)]
        sheet = Image.new('RGB', (sum(widths), sum(heights)), '#10141b')
        draw = ImageDraw.Draw(sheet)
        font_path = Path('C:/Windows/Fonts/segoeui.ttf')
        font = ImageFont.truetype(str(font_path), 24) if font_path.is_file() else ImageFont.load_default()
        for index, tile in enumerate(tiles):
            x, y = (widths[0] if index % 2 else 0), (heights[0] if index >= 2 else 0)
            draw.text((x + 8, y + 6), THEME_TITLES[index], font=font, fill='white')
            sheet.paste(tile, (x, y + 40))
        name = 'themes' + suffix + '.webp'
        sheet.save(output / name, lossless=True, method=6)
        old = safe_file(existing, name) if existing and (existing / name).exists() else None
        report['images'].append(dict(role='themes', language=language, file=name,
                                      **compare_images(output / name, old)))
    for item in report['images']:
        item['changed'] = item['meaningful_change']
    successful = {(item['role'], item['language']) for item in report['images']}
    for role in sorted(REQUIRED_DOC_ROLES):
        for language in (1, 14):
            if (role, language) not in successful:
                report['failures'].append({'role': role, 'language': language,
                                           'reason': 'missing required documentation role/language'})
    report['status'] = 'partial' if report['failures'] else 'complete'
    write_json(output / 'docs-report.json', report)
    return report


def overview(manifest, output, previous):
    esc = lambda value: html.escape(str(value), quote=True)
    lines = ['<!doctype html><html lang="zh-CN"><meta charset="utf-8">',
             '<meta name="viewport" content="width=device-width,initial-scale=1">',
             '<title>NR Nexus UI 快照</title><style>',
             'body{font:16px system-ui;margin:24px;background:#111820;color:#eef;line-height:1.5}',
             'a{color:#83d9ff} .grid{display:flex;flex-wrap:wrap;gap:16px}article{padding:12px;',
             'background:#1d2835;max-width:850px;min-width:250px;flex:1}img{max-width:100%;height:auto}',
             '.thumb{max-height:600px;object-fit:contain;object-position:top}pre{white-space:pre-wrap;',
             'overflow-wrap:anywhere}.compare{display:flex;gap:8px}.compare a{width:50%}</style>',
             '<h1>NR Nexus UI 快照</h1>', f'<p>{esc(CAVEAT)}</p>',
             f'<p>状态：{esc(manifest["status"])} · {esc(manifest["mode"])} · {esc(manifest["generated_utc"])}</p>',
             '<p>范围：' + esc(manifest.get('scope', 'complete')) + '</p>',
             '<p>源码：' + esc(manifest['source'].get('version', '')) + ' · '
             + esc(manifest['source'].get('commit', 'unknown'))
             + (' · 工作树有修改' if manifest['source'].get('changes') else ' · 工作树干净') + '</p>',
             '<p><a id="latest-link" href="../latest/index.html">最新版本</a> · '
             + ('<a class="relative-previous" href="../previous/index.html">上一版总览</a> · ' if previous else '')
             + '<a href="manifest.json">元数据</a></p>',
             '<details><summary>源码身份</summary><pre>' + esc(json.dumps(manifest['source'], ensure_ascii=False, indent=2)) + '</pre></details>']
    if manifest['failures']:
        lines += ['<h2>失败：此快照不可晋升</h2><pre>' + esc(json.dumps(manifest['failures'], ensure_ascii=False, indent=2)) + '</pre>']
    groups = {}
    for entry in manifest['entries']:
        page_name = PAGE_NAMES.get(entry['page'], entry.get('page_slug', entry['page']))
        groups.setdefault((str(page_name), str(entry['category'])), []).append(entry)
    for (page, category), entries in groups.items():
        lines.append(f'<h2>页签 {esc(page)} · {esc(category)}</h2><div class="grid">')
        for entry in sorted(entries, key=lambda item: (item['state'], item['id'])):
            src = esc(entry['image'])
            lines += [f'<article><h3>{esc(entry["id"])} · {esc(entry["state"])}</h3>',
                      f'<a href="{src}"><img loading="lazy" class="thumb" src="{src}" alt="{esc(entry["id"])}"></a>',
                      '<p>' + esc(' · '.join(str(entry[k]) for k in ('theme', 'language', 'font_size', 'width', 'height', 'kind'))) + '</p>',
                      '<p>交互：' + esc(entry['interaction']) + '</p>',
                      '<p>限制：' + esc(entry.get('limitations', [])) + '</p>']
            if 'comparison' in entry:
                comparison = entry['comparison']
                description = ('像素一致' if comparison['pixel_identical'] else
                               ('尺寸变化' if comparison['comparison'] == 'dimensions_changed' else
                                f'变化像素 {comparison.get("changed_fraction", 0):.2%}'))
                lines.append('<p class="relative-previous">上一版比较：' + esc(description) + '</p>')
            if previous:
                try:
                    safe_file(previous, 'images/' + entry['id'] + '.webp')
                except ValueError:
                    pass
                else:
                    old = '../previous/images/' + entry['id'] + '.webp'
                    lines += ['<details class="relative-previous"><summary>与上一版并排查看（左旧 / 右新）</summary><div class="compare">',
                              f'<a href="{old}"><img loading="lazy" src="{old}" alt="上一版"></a>',
                              f'<a href="{src}"><img loading="lazy" src="{src}" alt="本版"></a></div></details>']
            lines.append('</article>')
        lines.append('</div>')
    lines += ['<h2>覆盖与限制</h2><pre>' + esc(json.dumps(manifest['coverage'], ensure_ascii=False, indent=2)) + '</pre>',
              '<pre>' + esc(json.dumps(manifest['limitations'], ensure_ascii=False, indent=2)) + '</pre>',
              # After rotation only latest has a preceding accessible snapshot.
              # An older gallery must not compare its images with itself.
              '<script>const p=location.pathname;'
              'if(/\\/previous\\/index\\.html$/.test(p)||p.includes("/history/"))'
              'document.querySelectorAll(".relative-previous").forEach(e=>e.remove());'
              'if(p.includes("/history/"))document.getElementById("latest-link").href="../../latest/index.html";'
              '</script></html>']
    (output / 'index.html').write_text('\n'.join(lines), encoding='utf-8')


def build_snapshot(captures, output, source, previous=None, mode='Quick', documentation=None,
                   existing_docs=None, renderer=None):
    """Build a fresh candidate; return manifest, even for incomplete/invalid captures.

    `source` is a source-state JSON path (or an already parsed object for callers).
    A partial manifest must never be promoted. No inputs or existing docs are changed.
    """
    captures, output = Path(captures), Path(output)
    if captures.is_dir():
        captures = captures / 'captures.json'
    previous = Path(previous) if previous else None
    existing_docs = Path(existing_docs) if existing_docs else None
    if mode not in ('Quick', 'Full'):
        raise ValueError('mode must be Quick or Full')
    if documentation:
        documentation = Path(documentation)
        if documentation.exists() or documentation.resolve() == output.resolve():
            raise ValueError('documentation must be a new candidate directory distinct from output')
        if existing_docs and documentation.resolve().is_relative_to(existing_docs.resolve()):
            raise ValueError('documentation candidates cannot be inside existing docs')
    output.mkdir(parents=True, exist_ok=False)
    images = output / 'images'
    images.mkdir()
    manifest = {'schema': SCHEMA, 'status': 'partial', 'mode': mode,
                'generated_utc': datetime.now(timezone.utc).isoformat().replace('+00:00', 'Z'),
                'synthetic': True, 'caveat': CAVEAT, 'source': {}, 'entries': [],
                'failures': [], 'limitations': [], 'coverage': {}}
    expected, seen, rendered = [], [], []
    try:
        manifest['source'] = source if isinstance(source, dict) else read_json(source)
        if not isinstance(manifest['source'], dict) or not manifest['source']:
            raise ValueError('source-state must be a nonempty object')
        manifest['scope'] = 'selected' if manifest['source'].get('selection') else 'complete'
        registry = read_json(captures)
        if not isinstance(registry, dict) or not registry.get('schema'):
            raise ValueError('registry schema missing')
        expected = registry['expected']
        if (not isinstance(expected, list) or not expected
                or any(not isinstance(v, str) or not SAFE_ID.fullmatch(v) for v in expected)
                or len(set(expected)) != len(expected)):
            raise ValueError('expected must be a nonempty unique list of safe IDs')
        if not isinstance(registry['captures'], list):
            raise ValueError('captures must be a list')
        manifest['limitations'] = registry.get('limitations', [])
        renderer = renderer or renderer_module()
        used_files = set()
        atlas_hashes = {}
        for entry in registry['captures']:
            identifier = entry.get('id') if isinstance(entry, dict) else None
            try:
                if not isinstance(identifier, str) or not SAFE_ID.fullmatch(identifier):
                    raise ValueError('unsafe capture ID')
                if identifier in seen:
                    raise ValueError('duplicate capture ID')
                seen.append(identifier)
                if identifier not in expected:
                    raise ValueError('unexpected capture ID')
                path = safe_file(captures.parent, entry['file'])
                if path.resolve() in used_files:
                    raise ValueError('multiple IDs reference the same mesh')
                used_files.add(path.resolve())
                mesh = read_json(path)
                check_entry(entry, mesh)
                atlas = renderer.capture_atlas(path, mesh)
                safe_file(captures.parent, atlas.relative_to(captures.parent).as_posix())
                if (images / (path.stem + '.webp')).exists():
                    raise ValueError('render output name collision')
                result = renderer.render(path, images, 'webp')
                # Compatibility with the renderer before its return-Path extension.
                result = Path(result) if result is not None else images / (path.stem + '.webp')
                if not result.resolve().is_relative_to(images.resolve()):
                    raise ValueError('renderer output outside candidate images')
                target = images / (identifier + '.webp')
                with Image.open(result) as image:
                    image.load()
                    if image.size != (entry['width'], entry['height']):
                        raise ValueError('rendered dimensions differ from registry')
                    check_content(image)
                if result != target:
                    if target.exists():
                        raise ValueError('render output name collision')
                    result.rename(target)
                if atlas not in atlas_hashes:
                    atlas_hashes[atlas] = sha256(atlas)
                record = dict(entry, image='images/' + target.name, sha256=sha256(target),
                              mesh_sha256=sha256(path), atlas_sha256=atlas_hashes[atlas])
                if previous and (previous / 'images' / target.name).is_file():
                    old_image = safe_file(previous, 'images/' + target.name)
                    record['comparison'] = compare_images(target, old_image)
                if 'regions' not in record and 'regions' in mesh:
                    record['regions'] = mesh['regions']
                manifest['entries'].append(record)
                rendered.append(dict(record, _image=target))
            except (OSError, ValueError, KeyError, TypeError, AttributeError, OverflowError) as error:
                manifest['failures'].append({'id': identifier, 'reason': str(error)})
        missing = sorted(set(expected) - {entry['id'] for entry in manifest['entries']})
        if missing:
            manifest['failures'].append({'reason': 'missing expected captures', 'ids': missing})
        if documentation:
            docs = documentation_candidates(rendered, documentation, existing_docs, renderer)
            manifest['documentation'] = {'status': docs['status'], 'entries': len(docs['images']),
                                         'failures': docs['failures']}
            manifest['failures'].extend(dict(item, stage='documentation') for item in docs['failures'])
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        manifest['failures'].append({'reason': str(error)})
    manifest['coverage'] = {'expected': expected, 'rendered': [e['id'] for e in manifest['entries']],
                            'pages': dict(Counter(str(e['page']) for e in manifest['entries'])),
                            'states': dict(Counter(e['state'] for e in manifest['entries'])),
                            'categories': dict(Counter(e['category'] for e in manifest['entries']))}
    manifest['status'] = 'partial' if manifest['failures'] else 'complete'
    write_json(output / 'manifest.json', manifest)
    overview(manifest, output, previous)
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ('captures', 'output', 'source'):
        parser.add_argument('--' + argument, type=Path, required=True)
    parser.add_argument('--previous', type=Path)
    parser.add_argument('--mode', choices=('Quick', 'Full'), default='Quick')
    parser.add_argument('--documentation', type=Path)
    parser.add_argument('--existing-docs', type=Path)
    args = parser.parse_args(argv)
    try:
        result = build_snapshot(**vars(args))
    except (OSError, ValueError) as error:
        print('UI snapshot failed: ' + str(error), file=sys.stderr)
        return 1
    print(json.dumps({'status': result['status'], 'output': str(args.output.resolve()),
                      'failures': result['failures']}, ensure_ascii=False))
    return 0 if result['status'] == 'complete' else 1


if __name__ == '__main__':
    sys.exit(main())

"""Compose already reviewed production UI excerpts; does not capture or alter controls."""
import argparse
import hashlib
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]

def compose(language, output):
    suffix = '-zh' if language == 'zh' else ''
    source = ROOT / 'docs/images/ui'
    names = {'multipass': 'multipass', 'quality': 'quality', 'monitor': 'presets'}
    images = {name: Image.open(source / (filename + suffix + '.webp')).convert('RGB')
              for name, filename in names.items()}
    # Fixed, recorded crops of reviewed documentation images retain entire control groups.
    crops = {'multipass': (0, 0, 819, 928 if language == 'zh' else 925),
             'quality': (15, 87, 804, 486),
             'monitor': (186, 617, 633, 893) if language == 'zh' else (20, 617, 799, 856)}
    canvas = Image.new('RGB', (1664, 1016), '#08111b')
    draw = ImageDraw.Draw(canvas)
    font_path = Path('C:/Windows/Fonts/msyh.ttc') if language == 'zh' else Path('C:/Windows/Fonts/segoeui.ttf')
    font = ImageFont.truetype(str(font_path), 23)
    title = '模型、监视器与逐层控制' if language == 'zh' else 'Models, monitoring and per-pass control'
    draw.text((22, 12), title, font=font, fill='#dcebf3')
    placements = {'multipass': (835, 48), 'quality': (10, 82), 'monitor': (68, 550) if language == 'zh' else (10, 550)}
    section_font = ImageFont.truetype(str(font_path), 20)
    draw.text((22, 50), '模型与工作分辨率' if language == 'zh' else 'Model and working resolution', font=section_font, fill='#42d5e3')
    draw.text((22, 515), 'F11 运行监视器' if language == 'zh' else 'F11 runtime monitor', font=section_font, fill='#42d5e3')
    records = []
    for name, image in images.items():
        box = crops[name]
        assert box[2] <= image.width and box[3] <= image.height
        crop = image.crop(box)
        scale = 1.5 if name == 'monitor' and language == 'zh' else 1
        if scale != 1:
            crop = crop.resize((round(crop.width * scale), round(crop.height * scale)), Image.Resampling.LANCZOS)
        canvas.paste(crop, placements[name])
        records.append({'source': 'docs/images/ui/' + names[name] + suffix + '.webp',
                        'sha256': hashlib.sha256((source / (names[name] + suffix + '.webp')).read_bytes()).hexdigest(),
                        'crop': list(box), 'position': list(placements[name]), 'scale': scale})
    small = ImageFont.truetype(str(font_path), 17)
    note = 'v0.2.0 生产 UI 离屏节选 · 合成状态；非游戏画质或性能实测。点击查看大图。' if language == 'zh' else 'v0.2.0 production UI excerpts. Synthetic state; not game quality or performance measurements. Open for full size.'
    draw.text((22, 989), note, font=small, fill='#8da5b8')
    output.mkdir(parents=True, exist_ok=True)
    target = output / ('overview' + suffix + '.webp')
    canvas.save(target, 'WEBP', quality=93, method=6)
    return {'output': str(target), 'dimensions': list(canvas.size), 'quality': 93,
            'bytes': target.stat().st_size, 'sources': records}

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'artifacts/ui-snapshots/readme-collage')
    args = parser.parse_args()
    records = [compose(language, args.output) for language in ('zh', 'en')]
    (args.output / 'report.json').write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(records, ensure_ascii=False, indent=2))

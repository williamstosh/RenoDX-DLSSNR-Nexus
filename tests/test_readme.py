from pathlib import Path
import json
import re
import unittest
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
README = ROOT / "README.md"
GUIDE = ROOT / "docs/USER_GUIDE.md"


def anchors(text):
    found = []
    counts = {}
    for heading in re.finditer(r"(?m)^#{1,6}\s+(.+?)\s*#*\s*$", text):
        value = heading.group(1).lower()
        value = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", value)
        value = re.sub(r"<[^>]+>", "", value)
        value = re.sub(r"[^\w\-\s]", "", value)
        value = re.sub(r"\s", "-", value)
        count = counts.get(value, 0)
        counts[value] = count + 1
        found.append(value if count == 0 else f"{value}-{count}")
    found.extend(re.findall(r'<(?:a|h[1-6])\b[^>]*(?:id|name)=["\']([^"\']+)', text))
    return found


class ReadmeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = README.read_text(encoding="utf-8")

    def test_navigation_order_and_scene_selection(self):
        zh = self.text.index("## 简体中文")
        en = self.text.index("## English")
        self.assertLess(zh, en)
        expected = [
            "zh-index", "zh-comparisons", "zh-features", "zh-install", "zh-compatibility",
            "zh-feedback", "zh-build", "zh-sources", "en-index", "en-comparisons",
            "en-features", "en-install", "en-compatibility", "en-feedback", "en-build", "en-sources",
        ]
        ids = anchors(self.text)
        for target in expected:
            self.assertIn(target, ids)
        self.assertEqual(len(ids), len(set(ids)), "duplicate explicit or generated anchors")
        self.assertIn("/releases/latest", self.text)
        self.assertNotIn("docs/GAME_COMPARISONS.md", self.text)
        self.assertIn("更多场景", self.text)
        self.assertLess(self.text.index('<a id="zh-comparisons">'), self.text.index('<a id="zh-features">'))
        self.assertLess(self.text.index('<a id="en-comparisons">'), self.text.index('<a id="en-features">'))
        for language in ("zh", "en"):
            positions = [self.text.index(f'<a id="{language}-{key}">') for key in
                         ("comparisons", "features", "interface", "install", "compatibility", "feedback", "build", "sources")]
            self.assertEqual(positions, sorted(positions))

    def test_all_relative_links_and_anchors_resolve(self):
        docs = {"README.md": self.text, "docs/USER_GUIDE.md": GUIDE.read_text(encoding="utf-8")}
        for source, text in docs.items():
            for target in re.findall(r"\[[^\]]*\]\(([^\s)]+)\)", text):
                if re.match(r"(?:[a-zA-Z][a-zA-Z0-9+.-]*:|//)", target):
                    continue
                path, _, fragment = target.partition("#")
                resolved = (ROOT / source).parent.joinpath(unquote(path)).resolve() if path else ROOT / source
                self.assertTrue(resolved.is_file(), f"broken link in {source}: {target}")
                if fragment:
                    target_text = resolved.read_text(encoding="utf-8")
                    self.assertIn(unquote(fragment), anchors(target_text), f"broken anchor in {source}: {target}")

    def test_readme_images_are_paired_and_guide_references_stay_unchanged(self):
        for name in ("overview-zh.webp", "overview.webp"):
            self.assertIn(f"docs/images/ui/{name}", self.text)
            path = ROOT / "docs/images/ui" / name
            self.assertTrue(path.is_file())
            self.assertGreater(path.stat().st_size, 0)
        ui_dir = ROOT / "docs/images/ui"
        expected = {f"{name}{suffix}.webp" for name in ("quality", "multipass", "presets") for suffix in ("", "-zh")}
        guide = GUIDE.read_text(encoding="utf-8")
        actual = set(re.findall(r"images/ui/([a-z0-9-]+\.webp)", guide))
        self.assertTrue(expected.issubset(actual))
        for name in actual:
            self.assertTrue((ui_dir / name).is_file())

    def test_game_cards_use_complete_on_thumbnail_and_one_deep_link(self):
        catalog = json.loads((ROOT / "docs/compare/catalog.json").read_text(encoding="utf-8"))
        scenes = {scene["readme"]: scene for scene in catalog["scenes"]}
        for language in ("zh", "en"):
            block = self.text.split(f"<!-- game-compare:{language}:start -->")[1].split(f"<!-- game-compare:{language}:end -->")[0]
            cards = re.findall(r'\[!\[([^\]]+)\]\(docs/compare/([^\s)]+)\)\]\(([^\s)]+)\)', block)
            self.assertGreater(len(cards), 0)
            self.assertLessEqual(len(cards), 3)
            self.assertEqual(block.count("#scene="), len(cards))
            for caption, asset, link in cards:
                scene = scenes[asset]
                self.assertEqual(scene["status"], "approved")
                self.assertEqual(unquote(link.split("#scene=")[1]), scene["id"])
                self.assertIn(scene.get("scene_en", scene["scene"]) if language == "en" else scene["scene"], caption)
                self.assertTrue((ROOT / "docs/compare" / asset).is_file())


if __name__ == "__main__":
    unittest.main()

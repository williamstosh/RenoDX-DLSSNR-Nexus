from pathlib import Path
import tempfile
import unittest

from temporary_mod import TemporaryScriptMod


class TemporaryScriptModTests(unittest.TestCase):
    def make_mod(self, root, content="exec function test() {}\n"):
        source = root / "source" / "content" / "scripts" / "local"
        source.mkdir(parents=True)
        (source / "Bridge.ws").write_text(content, encoding="utf-8")
        return source.parents[2]

    def test_new_mod_is_removed_after_hash_verified_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            game = root / "game"
            game.mkdir()
            source = self.make_mod(root)
            lease = TemporaryScriptMod(game, source)
            files = lease.install()
            self.assertEqual(len(files), 1)
            self.assertTrue(files[0].is_file())
            lease.cleanup()
            self.assertFalse((game / "Mods").exists())

    def test_existing_identical_mod_is_used_but_not_removed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            game = root / "game"
            source = self.make_mod(root)
            target = game / "Mods" / "modNRNexusTestBridge" / "content" / "scripts" / "local"
            target.mkdir(parents=True)
            bridge = source / "content" / "scripts" / "local" / "Bridge.ws"
            (target / "Bridge.ws").write_text(bridge.read_text(encoding="utf-8"), encoding="utf-8")
            lease = TemporaryScriptMod(game, source)
            lease.install()
            lease.cleanup()
            self.assertTrue((target / "Bridge.ws").is_file())

    def test_conflicting_mod_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            game = root / "game"
            source = self.make_mod(root)
            target = game / "Mods" / "modNRNexusTestBridge"
            target.mkdir(parents=True)
            (target / "other.ws").write_text("user file", encoding="utf-8")
            lease = TemporaryScriptMod(game, source)
            with self.assertRaises(FileExistsError):
                lease.install()
            self.assertEqual((target / "other.ws").read_text(encoding="utf-8"), "user file")

    def test_modified_or_unexpected_files_are_preserved_for_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            game = root / "game"
            game.mkdir()
            source = self.make_mod(root)
            lease = TemporaryScriptMod(game, source)
            installed = lease.install()[0]
            (installed.parent / "generated.bin").write_bytes(b"unexpected")
            with self.assertRaises(RuntimeError):
                lease.cleanup()
            self.assertTrue(installed.is_file())
            self.assertTrue((installed.parent / "generated.bin").is_file())


if __name__ == "__main__":
    unittest.main()

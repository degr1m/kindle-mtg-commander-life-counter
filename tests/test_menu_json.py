import json
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
EXTENSION = ROOT / "extensions" / "mtg-life-counter"
MENU = EXTENSION / "menu.json"


class MenuJsonTest(unittest.TestCase):
    def test_kual_exposes_one_direct_commander_launcher(self):
        data = json.loads(MENU.read_text())
        self.assertEqual(len(data["items"]), 1)

        launcher = data["items"][0]
        self.assertEqual(launcher["name"], "MTG Commander Life Counter")
        self.assertEqual(launcher["action"], "/bin/sh")
        self.assertEqual(
            launcher["params"],
            "/mnt/us/extensions/mtg-life-counter/bin/launch.sh",
        )
        self.assertTrue(launcher["exitmenu"])
        self.assertNotIn("items", launcher)

    def test_launcher_binary_is_packaged_for_arm_without_runtime_state(self):
        binary = EXTENSION / "bin" / "mtg-life-counter"
        self.assertTrue(binary.is_file())
        header = binary.read_bytes()[:20]
        self.assertEqual(header[:4], b"\x7fELF")
        self.assertEqual(header[4], 1)  # ELF32
        self.assertEqual(int.from_bytes(header[18:20], "little"), 40)  # ARM
        self.assertFalse((EXTENSION / "data" / "state.txt").exists())
        first_icon = EXTENSION / "assets" / "first-player.png"
        self.assertTrue(first_icon.is_file())
        self.assertEqual(first_icon.read_bytes()[:8], b"\x89PNG\r\n\x1a\n")
        first_icon_source = EXTENSION / "assets" / "first-player.svg"
        self.assertTrue(first_icon_source.is_file())
        self.assertIn("licensed under MIT", first_icon_source.read_text())

    def test_extension_declares_dynamic_kual_menu(self):
        root = ET.parse(EXTENSION / "config.xml").getroot()
        menu = root.find("./menus/menu")

        self.assertIsNotNone(menu)
        assert menu is not None
        self.assertEqual(menu.get("type"), "json")
        self.assertEqual(menu.get("dynamic"), "true")
        self.assertEqual((menu.text or "").strip(), "menu.json")


if __name__ == "__main__":
    unittest.main()

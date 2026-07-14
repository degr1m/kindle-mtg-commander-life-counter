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
        self.assertFalse((EXTENSION / "assets" / "first-player.png").exists())
        self.assertFalse((EXTENSION / "assets" / "first-player.svg").exists())
        source = (ROOT / "native" / "src" / "main.c").read_text()
        self.assertIn("draw_first_triangles", source)
        self.assertNotIn("gdk_pixbuf", source)
        self.assertNotIn("FIRST_ICON", source)
        self.assertIn("event->type != GDK_BUTTON_PRESS", source)
        self.assertNotIn("DEVICE CHECK REQUIRED", source)
        self.assertIn("device_toggle_front_light", source)
        skull = source[source.index("static void draw_skull"):
                       source.index("static void draw_crown")]
        self.assertIn("cairo_new_path(cr);", skull)
        self.assertIn("minimum(skull.width, skull.height) * 1.176", source)
        self.assertIn("minimum(crown.width, crown.height) * 0.56", source)
        self.assertEqual(source.count("app->layout.marker_margin_x[player]"), 2)
        self.assertIn("static void draw_life_text", source)
        self.assertIn("const double life_size = current_size * 1.25;", source)
        self.assertIn("lipc-wait-event", source)
        self.assertIn("goingToScreenSaver,readyToSuspend,"
                      "outOfScreenSaver,wakeupFromSuspend", source)
        self.assertIn("G_SPAWN_SEARCH_PATH", source)
        self.assertIn("gtk_widget_set_sensitive(app->canvas, FALSE)", source)
        self.assertIn("gtk_widget_set_sensitive(app->canvas, settled)", source)
        self.assertIn("device_power_exit", source)
        self.assertIn("static volatile sig_atomic_t termination_signal", source)
        self.assertIn("sigaction(SIGTERM", source)
        self.assertIn("poll_termination", source)
        self.assertIn("setpgid(0, 0)", source)
        self.assertIn("power_restart_timer", source)
        self.assertIn("schedule_power_restart", source)
        self.assertIn("reconcile_power_state", source)
        self.assertIn("device_power_reconcile", source)
        self.assertIn("POWER_EVENT_LINE_MAX", source)
        self.assertIn("POWER_EVENT_READ_BUDGET", source)
        self.assertIn("g_io_channel_read_chars", source)
        self.assertNotIn("g_io_channel_read_line", source)
        self.assertIn("byte == '\\0'", source)
        self.assertIn("power_reconcile_timer", source)
        self.assertIn("schedule_power_reconcile", source)
        self.assertIn("POWER_RESTART_MAX_MS", source)
        self.assertIn("POWER_RECONCILE_WATCHDOG_MS", source)
        self.assertIn("power_reconcile_due_us", source)
        self.assertIn("g_get_monotonic_time", source)
        self.assertIn("reject_power_event_record", source)
        self.assertIn("schedule_power_reconcile_now", source)
        self.assertIn("schedule_power_watchdog", source)
        self.assertIn("DEVICE_POWER_OBSERVED_UNKNOWN", source)
        startup = source[source.index("gtk_widget_show_all(app.window)"):
                         source.index("gtk_main()")]
        self.assertIn("reconcile_power_state(&app)", startup)
        handler = source[source.index("static void handle_power_event"):
                         source.index("static void reconcile_power_state")]
        self.assertNotIn("device_power_sleep", handler)
        self.assertNotIn("device_power_wake", handler)
        self.assertNotIn("power_restart_delay_ms", handler)
        self.assertIn("POWER_RESTART_STABLE_MS", source)
        self.assertIn("mark_power_watch_stable", source)
        collector = source[source.index("static void finish_power_event_line"):
                           source.index("static gboolean on_power_event_output")]
        self.assertIn("schedule_power_reconcile_now", collector)
        cleanup = source[source.index("static void terminate_power_child"):
                         source.index("static void handle_power_event")]
        self.assertNotIn("WNOHANG", cleanup)
        self.assertNotIn("g_source_remove(app->power_child_watch)", cleanup)
        watcher = source[source.index("static gboolean start_power_watch(",
                                      source.index("static void handle_power_event")):
                         source.index("static gboolean restart_power_watch")]
        self.assertLess(watcher.index("g_child_watch_add"),
                        watcher.index("g_io_channel_set_flags"))
        watch_failure = watcher[watcher.index("if (!app->power_child_watch)"):
                                watcher.index("app->power_channel =")]
        self.assertIn("close(output_fd)", watch_failure)
        self.assertIn("EXIT FAILED", source)
        self.assertIn("if (device_power_exit", source)
        launcher_source = (EXTENSION / "bin" / "launch.sh").read_text()
        self.assertNotIn("preventScreenSaver", launcher_source)
        self.assertIn("kill -KILL", launcher_source)
        meson = (ROOT / "native" / "meson.build").read_text()
        self.assertIn("src/device.c", meson)

    def test_extension_declares_dynamic_kual_menu(self):
        root = ET.parse(EXTENSION / "config.xml").getroot()
        self.assertEqual(root.findtext("./information/version"), "2.0.0")
        menu = root.find("./menus/menu")

        self.assertIsNotNone(menu)
        assert menu is not None
        self.assertEqual(menu.get("type"), "json")
        self.assertEqual(menu.get("dynamic"), "true")
        self.assertEqual((menu.text or "").strip(), "menu.json")


if __name__ == "__main__":
    unittest.main()

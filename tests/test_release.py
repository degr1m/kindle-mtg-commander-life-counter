import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import tempfile
import time
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
EXPECTED_ARCHIVE = [
    "extensions/mtg-life-counter/",
    "extensions/mtg-life-counter/bin/",
    "extensions/mtg-life-counter/bin/launch.sh",
    "extensions/mtg-life-counter/bin/mtg-life-counter",
    "extensions/mtg-life-counter/config.xml",
    "extensions/mtg-life-counter/menu.json",
]


class ReleaseFixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "repo"
        scripts = self.root / "scripts"
        extension = self.root / "extensions" / "mtg-life-counter"
        (extension / "bin").mkdir(parents=True)
        scripts.mkdir(parents=True)
        for name in ("package-release.sh", "install-to-kindle.sh"):
            source = ROOT / "scripts" / name
            shutil.copy2(source, scripts / name)
        helper = ROOT / "scripts" / "release_tree.py"
        if helper.exists():
            shutil.copy2(helper, scripts / helper.name)

        (extension / "bin" / "launch.sh").write_text("#!/bin/sh\nexit 0\n")
        (extension / "bin" / "mtg-life-counter").write_bytes(b"ELF ARM fixture\n")
        (extension / "config.xml").write_text("<version>2.0.0</version>\n")
        (extension / "menu.json").write_text('{"items": []}\n')
        os.chmod(extension / "bin" / "launch.sh", 0o755)
        os.chmod(extension / "bin" / "mtg-life-counter", 0o755)
        self.extension = extension

    def tearDown(self):
        self.temporary.cleanup()

    def run_script(self, name, *arguments, timezone="UTC", extra_environment=None):
        environment = os.environ.copy()
        environment["TZ"] = timezone
        if extra_environment is not None:
            environment.update(extra_environment)
        return subprocess.run(
            [str(self.root / "scripts" / name), *map(str, arguments)],
            cwd=self.root,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )


class PackageReleaseTest(ReleaseFixture):
    def test_rejects_unexpected_source_content(self):
        (self.extension / "private-notes.txt").write_text("must not ship\n")

        result = self.run_script("package-release.sh", "2.0.0")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertFalse(
            (self.root / "dist" / "mtg-life-counter-v2.0.0-kindle.zip").exists()
        )

    def test_rejects_symlink_without_touching_target(self):
        target = self.root / "outside-config.xml"
        target.write_text("private\n")
        os.chmod(target, 0o600)
        (self.extension / "config.xml").unlink()
        (self.extension / "config.xml").symlink_to(target)

        result = self.run_script("package-release.sh", "2.0.0")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o600)

    def test_archive_is_exact_and_reproducible_across_timezones(self):
        first = self.run_script("package-release.sh", "2.0.0", timezone="UTC")
        self.assertEqual(first.returncode, 0, first.stdout)
        archive = self.root / "dist" / "mtg-life-counter-v2.0.0-kindle.zip"
        first_bytes = archive.read_bytes()

        second = self.run_script(
            "package-release.sh", "2.0.0", timezone="Pacific/Honolulu"
        )
        self.assertEqual(second.returncode, 0, second.stdout)
        self.assertEqual(archive.read_bytes(), first_bytes)

        with zipfile.ZipFile(archive) as package:
            self.assertEqual(package.namelist(), EXPECTED_ARCHIVE)
            modes = {
                info.filename: (info.external_attr >> 16) & 0o777
                for info in package.infolist()
            }
        self.assertEqual(modes[EXPECTED_ARCHIVE[0]], 0o755)
        self.assertEqual(modes[EXPECTED_ARCHIVE[1]], 0o755)
        self.assertEqual(modes[EXPECTED_ARCHIVE[2]], 0o755)
        self.assertEqual(modes[EXPECTED_ARCHIVE[3]], 0o755)
        self.assertEqual(modes[EXPECTED_ARCHIVE[4]], 0o644)
        self.assertEqual(modes[EXPECTED_ARCHIVE[5]], 0o644)


class MountedInstallTest(ReleaseFixture):
    def existing_install(self):
        app = self.root / "kindle" / "extensions" / "mtg-life-counter"
        (app / "data").mkdir(parents=True)
        (app / "data" / "state.txt").write_text("preserved-state\n")
        (app / "config.xml").write_text("old-config\n")
        return app

    def test_update_preserves_existing_data_and_excludes_local_runtime(self):
        app = self.existing_install()
        (self.extension / "data").mkdir()
        (self.extension / "data" / "state.txt").write_text("local-state\n")
        (self.extension / "app.log").write_text("local-log\n")

        result = self.run_script("install-to-kindle.sh", self.root / "kindle")

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual((app / "data" / "state.txt").read_text(), "preserved-state\n")
        self.assertFalse((app / "app.log").exists())
        self.assertIn("2.0.0", (app / "config.xml").read_text())

    def test_invalid_source_leaves_existing_install_untouched(self):
        app = self.existing_install()
        (self.extension / "menu.json").unlink()

        result = self.run_script("install-to-kindle.sh", self.root / "kindle")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual((app / "config.xml").read_text(), "old-config\n")
        self.assertEqual((app / "data" / "state.txt").read_text(), "preserved-state\n")

    def test_running_app_lock_refuses_without_touching_install(self):
        app = self.existing_install()
        (app / "data" / "app.lock").mkdir()

        result = self.run_script("install-to-kindle.sh", self.root / "kindle")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual((app / "config.xml").read_text(), "old-config\n")
        self.assertEqual(list(app.parent.glob(".mtg-life-counter.new*")), [])

    def test_stale_incoming_is_removed_before_update(self):
        app = self.existing_install()
        incoming = app.parent / ".mtg-life-counter.new"
        incoming.mkdir()
        (incoming / "partial").write_text("stale\n")

        result = self.run_script("install-to-kindle.sh", self.root / "kindle")

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse(incoming.exists())
        self.assertIn("2.0.0", (app / "config.xml").read_text())
        self.assertEqual((app / "data" / "state.txt").read_text(), "preserved-state\n")

    def test_failed_copy_removes_partial_incoming(self):
        app = self.existing_install()
        tools = self.root / "fake-tools"
        tools.mkdir()
        fake_cp = tools / "cp"
        fake_cp.write_text(
            "#!/bin/sh\n"
            "mkdir -p \"$2\"\n"
            "printf partial > \"$2/partial\"\n"
            "exit 1\n"
        )
        fake_cp.chmod(0o755)

        result = self.run_script(
            "install-to-kindle.sh",
            self.root / "kindle",
            extra_environment={"PATH": f"{tools}{os.pathsep}{os.environ['PATH']}"},
        )

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual((app / "config.xml").read_text(), "old-config\n")
        self.assertEqual(list(app.parent.glob(".mtg-life-counter.new*")), [])

    def test_signal_after_backup_restores_live_install(self):
        app = self.existing_install()
        tools = self.root / "signal-tools"
        tools.mkdir()
        state = tools / "mv-called"
        fake_mv = tools / "mv"
        fake_mv.write_text(
            "#!/bin/sh\n"
            f"if [ ! -e '{state}' ]; then\n"
            f"  : > '{state}'\n"
            "  /bin/mv \"$@\"\n"
            "  parent=$(ps -o ppid= -p $$)\n"
            "  kill -TERM $parent\n"
            "  sleep 1\n"
            "  exit 0\n"
            "fi\n"
            "exec /bin/mv \"$@\"\n"
        )
        fake_mv.chmod(0o755)

        result = self.run_script(
            "install-to-kindle.sh",
            self.root / "kindle",
            extra_environment={"PATH": f"{tools}{os.pathsep}{os.environ['PATH']}"},
        )

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual((app / "config.xml").read_text(), "old-config\n")
        self.assertEqual((app / "data" / "state.txt").read_text(), "preserved-state\n")
        self.assertEqual(list(app.parent.glob(".mtg-life-counter.new*")), [])
        self.assertEqual(list(app.parent.glob(".mtg-life-counter.old*")), [])

    def test_locked_live_target_is_not_replaced_by_stale_backup(self):
        app = self.existing_install()
        (app / "data" / "app.lock").mkdir()
        backup = app.parent / ".mtg-life-counter.old"
        (backup / "data").mkdir(parents=True)
        (backup / "data" / "state.txt").write_text("backup-state\n")
        (backup / "config.xml").write_text("backup-config\n")

        result = self.run_script("install-to-kindle.sh", self.root / "kindle")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual((app / "data" / "state.txt").read_text(), "preserved-state\n")
        self.assertTrue(backup.exists())

    def test_post_action_promotion_failure_restores_previous_target(self):
        app = self.existing_install()
        tools = self.root / "post-action-tools"
        tools.mkdir()
        incoming = app.parent / ".mtg-life-counter.new"
        fake_mv = tools / "mv"
        fake_mv.write_text(
            "#!/bin/sh\n"
            f"if [ \"$1\" = '{incoming}' ] && [ \"$2\" = '{app}' ]; then\n"
            "  /bin/mv \"$@\"\n"
            "  exit 1\n"
            "fi\n"
            "exec /bin/mv \"$@\"\n"
        )
        fake_mv.chmod(0o755)

        result = self.run_script(
            "install-to-kindle.sh", self.root / "kindle",
            extra_environment={"PATH": f"{tools}{os.pathsep}{os.environ['PATH']}"},
        )

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual((app / "config.xml").read_text(), "old-config\n")
        self.assertEqual((app / "data" / "state.txt").read_text(), "preserved-state\n")
        self.assertFalse((app / ".mtg-life-counter.old").exists())

    def test_signal_during_committed_backup_cleanup_preserves_promoted_target(self):
        app = self.existing_install()
        tools = self.root / "cleanup-signal-tools"
        tools.mkdir()
        backup = app.parent / ".mtg-life-counter.old"
        state = tools / "backup-delete-attempted"
        fake_rm = tools / "rm"
        fake_rm.write_text(
            "#!/bin/sh\n"
            f"if [ \"${{@:$#}}\" = '{backup}' ] && [ ! -e '{state}' ]; then\n"
            f"  : > '{state}'\n"
            f"  /bin/rm -f '{backup}/data/state.txt'\n"
            "  parent=$(ps -o ppid= -p $$)\n"
            "  kill -TERM $parent\n"
            "  exit 1\n"
            "fi\n"
            "exec /bin/rm \"$@\"\n"
        )
        fake_rm.chmod(0o755)

        result = self.run_script(
            "install-to-kindle.sh", self.root / "kindle",
            extra_environment={"PATH": f"{tools}{os.pathsep}{os.environ['PATH']}"},
        )

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("2.0.0", (app / "config.xml").read_text())
        self.assertEqual((app / "data" / "state.txt").read_text(), "preserved-state\n")

    def test_symlinked_extensions_parent_is_refused(self):
        kindle = self.root / "kindle"
        outside = self.root / "outside"
        kindle.mkdir()
        outside.mkdir()
        (kindle / "extensions").symlink_to(outside, target_is_directory=True)

        result = self.run_script("install-to-kindle.sh", kindle)

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertFalse((outside / "mtg-life-counter").exists())

    def test_fifo_backup_is_refused(self):
        app = self.existing_install()
        os.mkfifo(app.parent / ".mtg-life-counter.old")

        result = self.run_script("install-to-kindle.sh", self.root / "kindle")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual((app / "config.xml").read_text(), "old-config\n")
        self.assertEqual((app / "data" / "state.txt").read_text(), "preserved-state\n")

    def test_fifo_data_is_refused(self):
        app = self.existing_install()
        shutil.rmtree(app / "data")
        os.mkfifo(app / "data")

        result = self.run_script("install-to-kindle.sh", self.root / "kindle")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual((app / "config.xml").read_text(), "old-config\n")

    def test_nested_data_symlink_is_refused(self):
        app = self.existing_install()
        external = self.root / "external-state"
        external.write_text("private\n")
        (app / "data" / "external").symlink_to(external)

        result = self.run_script("install-to-kindle.sh", self.root / "kindle")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual((app / "config.xml").read_text(), "old-config\n")

    def test_symlinked_data_does_not_delete_external_installer_lock(self):
        app = self.existing_install()
        shutil.rmtree(app / "data")
        external = self.root / "external-data"
        lock = external / "install.lock"
        lock.mkdir(parents=True)
        (lock / "owner").write_text("999999\n")
        (external / "sentinel").write_text("private\n")
        (app / "data").symlink_to(external, target_is_directory=True)

        result = self.run_script("install-to-kindle.sh", self.root / "kindle")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertTrue(lock.is_dir())
        self.assertEqual((external / "sentinel").read_text(), "private\n")

    def test_live_mounted_installer_guard_refuses_second_installer(self):
        app = self.existing_install()
        guard = app.parent / ".mtg-life-counter.installer"
        guard.mkdir()
        (guard / "owner").write_text(f"{os.getpid()}\n")

        result = self.run_script("install-to-kindle.sh", self.root / "kindle")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual((app / "config.xml").read_text(), "old-config\n")
        self.assertEqual((guard / "owner").read_text(), f"{os.getpid()}\n")

    def test_stale_backup_keeps_guard_and_cleanup_preserves_foreign_guard(self):
        app = self.existing_install()
        backup = app.parent / ".mtg-life-counter.old"
        shutil.copytree(self.extension, backup)
        (backup / "data").mkdir()
        (backup / "data" / "state.txt").write_text("backup-state\n")
        tools = self.root / "guard-window-tools"
        tools.mkdir()
        paused = tools / "paused"
        proceed = tools / "proceed"
        fake_cp = tools / "cp"
        fake_cp.write_text(
            "#!/bin/sh\n"
            f": > '{paused}'\n"
            f"while [ ! -e '{proceed}' ]; do sleep 0.02; done\n"
            "exit 1\n"
        )
        fake_cp.chmod(0o755)
        environment = os.environ.copy()
        environment["PATH"] = f"{tools}{os.pathsep}{environment['PATH']}"
        process = subprocess.Popen(
            [str(self.root / "scripts" / "install-to-kindle.sh"),
             str(self.root / "kindle")],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and not paused.exists():
                if process.poll() is not None:
                    break
                time.sleep(0.02)
            self.assertTrue(paused.exists())
            guard = app / "data" / "app.lock.guard"
            self.assertEqual((guard / "pid").read_text(), "1\n")
            self.assertEqual((guard / "installer-owner").read_text(), f"{process.pid}\n")

            shutil.rmtree(guard)
            guard.mkdir()
            (guard / "pid").write_text(f"{os.getpid()}\n")
            proceed.touch()
            output, _ = process.communicate(timeout=5)

            self.assertNotEqual(process.returncode, 0, output)
            self.assertEqual((guard / "pid").read_text(), f"{os.getpid()}\n")
            self.assertFalse((guard / "installer-owner").exists())
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)

    def test_installer_uses_launcher_guard_during_lock_publication(self):
        source = (ROOT / "scripts" / "install-to-kindle.sh").read_text()
        guard = source.index('mkdir "$root/data/app.lock.guard"')
        owner = source.index('app.lock.guard/installer-owner', guard)
        reclaim = source.index('reclaim_stale_volume_lock "$target/data/install.lock"')
        publish = source.index('mkdir "$root/data/install.lock"')
        recheck = source.index("Life counter started during installer lock publication")
        commit = source.index("  VOLUME_COMMITTED=1")
        release = source.index("release_volume_guard", commit)

        self.assertLess(guard, reclaim)
        self.assertLess(guard, owner)
        self.assertLess(owner, reclaim)
        self.assertLess(reclaim, publish)
        self.assertLess(publish, recheck)
        self.assertLess(recheck, commit)
        self.assertLess(commit, release)


class LauncherSignalTest(unittest.TestCase):
    def test_install_lock_blocks_application_launch(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "mtg-life-counter"
            (app / "bin").mkdir(parents=True)
            (app / "data" / "install.lock").mkdir(parents=True)
            launcher = app / "bin" / "launch.sh"
            shutil.copy2(
                ROOT / "extensions" / "mtg-life-counter" / "bin" / "launch.sh",
                launcher,
            )
            child = app / "bin" / "mtg-life-counter"
            child.write_text(
                "#!/bin/sh\n"
                f": > '{app / 'started'}'\n"
            )
            child.chmod(0o755)
            environment = os.environ.copy()
            environment["MTG_APP_DIR"] = str(app)

            result = subprocess.run(
                [str(launcher)], env=environment,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=5,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((app / "started").exists())

    def test_term_forwards_and_reaps_the_app_process_group(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "mtg-life-counter"
            (app / "bin").mkdir(parents=True)
            launcher = app / "bin" / "launch.sh"
            shutil.copy2(
                ROOT / "extensions" / "mtg-life-counter" / "bin" / "launch.sh",
                launcher,
            )
            child = app / "bin" / "mtg-life-counter"
            child.write_text(
                "#!/usr/bin/env python3\n"
                "import os, signal, sys, time\n"
                "root = sys.argv[1].rsplit('/data/', 1)[0]\n"
                "os.setpgid(0, 0)\n"
                "descendant = os.fork()\n"
                "if descendant == 0:\n"
                "    signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                "    open(root + '/descendant', 'w').write(str(os.getpid()))\n"
                "    time.sleep(4)\n"
                "    open(root + '/leaked', 'w').write('yes')\n"
                "    os._exit(0)\n"
                "def stop(signum, frame):\n"
                "    open(root + '/term-received', 'w').write(str(signum))\n"
                "    raise SystemExit(0)\n"
                "signal.signal(signal.SIGTERM, stop)\n"
                "open(root + '/started', 'w').write(str(os.getpid()))\n"
                "while True: time.sleep(1)\n"
            )
            child.chmod(0o755)
            environment = os.environ.copy()
            environment["MTG_APP_DIR"] = str(app)
            environment["MTG_SHUTDOWN_GRACE_STEPS"] = "1"
            process = subprocess.Popen(
                [str(launcher)],
                env=environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline and not (app / "started").exists():
                    if process.poll() is not None:
                        break
                    time.sleep(0.02)
                self.assertTrue((app / "started").exists())
                process.send_signal(signal.SIGTERM)
                self.assertEqual(process.wait(timeout=5), 143)
                self.assertTrue((app / "term-received").exists())
                self.assertFalse((app / "data" / "app.lock").exists())
                time.sleep(0.2)
                self.assertFalse((app / "leaked").exists())
                descendant = int((app / "descendant").read_text())
                with self.assertRaises(ProcessLookupError):
                    os.kill(descendant, 0)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_normal_exit_reaps_group_before_releasing_lock(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "mtg-life-counter"
            (app / "bin").mkdir(parents=True)
            launcher = app / "bin" / "launch.sh"
            shutil.copy2(
                ROOT / "extensions" / "mtg-life-counter" / "bin" / "launch.sh",
                launcher,
            )
            child = app / "bin" / "mtg-life-counter"
            child.write_text(
                "#!/usr/bin/env python3\n"
                "import os, signal, sys, time\n"
                "root = sys.argv[1].rsplit('/data/', 1)[0]\n"
                "os.setpgid(0, 0)\n"
                "descendant = os.fork()\n"
                "if descendant == 0:\n"
                "    signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                "    open(root + '/descendant', 'w').write(str(os.getpid()))\n"
                "    time.sleep(2)\n"
                "    open(root + '/leaked', 'w').write('yes')\n"
                "    os._exit(0)\n"
                "os._exit(0)\n"
            )
            child.chmod(0o755)
            environment = os.environ.copy()
            environment["MTG_APP_DIR"] = str(app)
            environment["MTG_SHUTDOWN_GRACE_STEPS"] = "1"

            result = subprocess.run(
                [str(launcher)], env=environment,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=5,
            )

            self.assertEqual(result.returncode, 0)
            self.assertFalse((app / "data" / "app.lock").exists())
            time.sleep(0.2)
            self.assertFalse((app / "leaked").exists())
            descendant = int((app / "descendant").read_text())
            with self.assertRaises(ProcessLookupError):
                os.kill(descendant, 0)

    def test_concurrent_lock_publication_starts_only_one_app(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "mtg-life-counter"
            tools = Path(temporary) / "tools"
            (app / "bin").mkdir(parents=True)
            tools.mkdir()
            launcher = app / "bin" / "launch.sh"
            shutil.copy2(
                ROOT / "extensions" / "mtg-life-counter" / "bin" / "launch.sh",
                launcher,
            )
            child = app / "bin" / "mtg-life-counter"
            child.write_text(
                "#!/usr/bin/env python3\n"
                "import os, sys, time\n"
                "root = sys.argv[1].rsplit('/data/', 1)[0]\n"
                "os.setpgid(0, 0)\n"
                "with open(root + '/starts', 'a') as stream:\n"
                "    stream.write(str(os.getpid()) + '\\n')\n"
                "time.sleep(2)\n"
            )
            child.chmod(0o755)
            fake_mkdir = tools / "mkdir"
            fake_mkdir.write_text(
                "#!/bin/sh\n"
                "/bin/mkdir \"$@\" || exit $?\n"
                "case \"$*\" in\n"
                "  *'/data/app.lock') sleep 1 ;;\n"
                "esac\n"
            )
            fake_mkdir.chmod(0o755)
            environment = os.environ.copy()
            environment["MTG_APP_DIR"] = str(app)
            environment["MTG_SHUTDOWN_GRACE_STEPS"] = "1"
            environment["PATH"] = f"{tools}{os.pathsep}{environment['PATH']}"
            first = subprocess.Popen(
                [str(launcher)], env=environment,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            second = None
            try:
                lock = app / "data" / "app.lock"
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline and not lock.exists():
                    time.sleep(0.01)
                self.assertTrue(lock.exists())
                second = subprocess.Popen(
                    [str(launcher)], env=environment,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                )
                second.wait(timeout=4)
                deadline = time.monotonic() + 3
                starts = app / "starts"
                while time.monotonic() < deadline and not starts.exists():
                    time.sleep(0.02)
                self.assertTrue(starts.exists())
                self.assertEqual(len(starts.read_text().splitlines()), 1)
            finally:
                for process in (first, second):
                    if process is not None and process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                        process.wait(timeout=5)

    def test_invalid_zero_lock_pid_is_recovered_without_process_selector(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "mtg-life-counter"
            (app / "bin").mkdir(parents=True)
            lock = app / "data" / "app.lock"
            lock.mkdir(parents=True)
            (lock / "pid").write_text("0\n")
            launcher = app / "bin" / "launch.sh"
            shutil.copy2(
                ROOT / "extensions" / "mtg-life-counter" / "bin" / "launch.sh",
                launcher,
            )
            child = app / "bin" / "mtg-life-counter"
            child.write_text("#!/bin/sh\nexit 0\n")
            child.chmod(0o755)
            environment = os.environ.copy()
            environment["MTG_APP_DIR"] = str(app)

            result = subprocess.run(
                [str(launcher)], env=environment,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=5,
            )

            self.assertEqual(result.returncode, 0)
            self.assertFalse(lock.exists())


class NativeShutdownSourceTest(unittest.TestCase):
    def test_window_delete_is_gated_before_destroy(self):
        source = (ROOT / "native" / "src" / "main.c").read_text()
        delete_start = source.index("static gboolean on_window_delete")
        destroy_start = source.index("static void on_window_destroy")
        poll_start = source.index("static gboolean poll_termination")
        delete_handler = source[delete_start:destroy_start]
        destroy_handler = source[destroy_start:poll_start]

        self.assertIn("device_power_exit", delete_handler)
        self.assertIn("return TRUE", delete_handler)
        self.assertNotIn("device_power_exit", destroy_handler)
        self.assertIn('"delete-event"', source)


class MtpInstallerSourceTest(unittest.TestCase):
    def test_mtp_installer_uses_verified_transactional_boundaries(self):
        source = (ROOT / "scripts" / "mtp-install.c").read_text()
        self.assertNotIn("LIBMTP_Get_First_Device", source)
        self.assertNotIn("LIBMTP_Open_Raw_Device_Uncached", source)
        self.assertNotIn("LIBMTP_Set_Object_Filename", source)
        self.assertNotIn("find_any", source)
        self.assertIn("LIBMTP_Detect_Raw_Devices", source)
        self.assertIn("LIBMTP_Open_Raw_Device(&raw[selected])", source)
        self.assertIn("get_fresh_children", source)
        destroy_start = source.index("static void destroy_file_list")
        destroy_end = source.index("static LIBMTP_mtpdevice_t", destroy_start)
        destroy = source[destroy_start:destroy_end]
        self.assertIn("while (files)", destroy)
        self.assertIn("LIBMTP_file_t *next = files->next", destroy)
        self.assertIn("LIBMTP_destroy_file_t(files)", destroy)
        self.assertIn("files = next", destroy)
        fresh_start = source.index("static LIBMTP_file_t *get_fresh_children")
        fresh_end = source.index("static int verify_promoted_code", fresh_start)
        fresh = source[fresh_start:fresh_end]
        self.assertLess(fresh.index("int cached = device->cached"),
                        fresh.index("device->cached = 0"))
        self.assertLess(fresh.index("device->cached = 0"),
                        fresh.index("LIBMTP_Get_Files_And_Folders"))
        self.assertLess(fresh.index("LIBMTP_Get_Files_And_Folders"),
                        fresh.index("device->cached = cached"))
        self.assertGreaterEqual(source.count("destroy_file_list("), 12)
        self.assertIn("AMAZON_VENDOR_ID", source)
        self.assertIn("find_unique_root_child", source)
        self.assertIn(".mtg-life-counter.new", source)
        self.assertIn("LIBMTP_Move_Object", source)
        self.assertIn("rollback_cutover", source)
        self.assertIn("delete_stage_objects", source)
        self.assertIn("hash_remote_file", source)
        self.assertIn("app.lock", source)
        self.assertIn("data_snapshot_equal", source)

    def test_mtp_cutover_is_rename_free_and_backup_backed(self):
        source = (ROOT / "scripts" / "mtp-install.c").read_text()

        self.assertNotIn("LIBMTP_Set_Folder_Name", source)
        self.assertIn("backup_old_code", source)
        self.assertIn("restore_remote_journal", source)
        self.assertIn("purge_live_code", source)
        self.assertNotIn("old_launch_deleted", source)
        self.assertIn("verify_promoted_code", source)
        self.assertIn("mkdtemp", source)

    def test_mtp_byte_comparison_closes_both_streams(self):
        source = (ROOT / "scripts" / "mtp-install.c").read_text()

        self.assertIn("int left_close = fclose(left);", source)
        self.assertIn("int right_close = fclose(right);", source)

    def test_mtp_failures_remain_recoverable_across_reconnect(self):
        source = (ROOT / "scripts" / "mtp-install.c").read_text()

        self.assertIn("BACKUP_NAME", source)
        self.assertIn("recover_stale_transaction", source)
        self.assertIn("find_unique_direct_folder", source)
        self.assertIn("acquire_installer_lock", source)
        self.assertIn("verify_installer_lock", source)
        self.assertIn("reclaim_stale_installer_lock", source)
        self.assertGreaterEqual(source.count("verify_installer_lock("), 4)
        self.assertIn("retained_item", source)
        self.assertIn("rollback_incomplete", source)
        self.assertIn("keep_local_backup", source)
        self.assertIn("HOST_LOCK_PATH", source)
        self.assertIn("fcntl(host_lock, F_SETLK, &lease)", source)

    def test_mtp_ready_cleanup_and_stale_stage_fail_closed(self):
        source = (ROOT / "scripts" / "mtp-install.c").read_text()
        cleanup_start = source.index("static int delete_stage_objects")
        cleanup_end = source.index("static int delete_remote_files")
        cleanup = source[cleanup_start:cleanup_end]

        self.assertLess(
            cleanup.index("stage->ready && LIBMTP_Delete_Object"),
            cleanup.index("stage->launch"),
        )
        self.assertNotIn("!backup_folder && existing", source)
        self.assertIn("keep_local_backup = backup.root[0] != '\\0';", source)


if __name__ == "__main__":
    unittest.main()

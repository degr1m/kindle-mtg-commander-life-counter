#!/usr/bin/env python3
import argparse
import os
from pathlib import Path
import shutil
import stat
import tempfile
import zipfile

APP_ROOT = Path("extensions/mtg-life-counter")
DIRECTORIES = (
    (Path("."), 0o755),
    (Path("bin"), 0o755),
)
FILES = (
    (Path("bin/launch.sh"), 0o755),
    (Path("bin/mtg-life-counter"), 0o755),
    (Path("config.xml"), 0o644),
    (Path("menu.json"), 0o644),
)
RUNTIME_FILES = {Path("app.log"), Path("app.log.old")}
RUNTIME_DIRECTORY = Path("data")
ARCHIVE_TIME = (2026, 7, 14, 0, 0, 0)


def fail(message):
    raise ValueError(message)


def checked_kind(path):
    metadata = path.lstat()
    if stat.S_ISLNK(metadata.st_mode):
        fail(f"Symlink is not allowed: {path}")
    if stat.S_ISDIR(metadata.st_mode):
        return "directory"
    if stat.S_ISREG(metadata.st_mode):
        return "file"
    fail(f"Special file is not allowed: {path}")


def validate_source(source, allow_runtime):
    source = Path(source)
    if checked_kind(source) != "directory":
        fail(f"Release source is not a directory: {source}")

    expected_directories = {path for path, _ in DIRECTORIES}
    expected_files = {path for path, _ in FILES}
    actual_directories = {Path(".")}
    actual_files = set()

    for root, directories, files in os.walk(source, topdown=True, followlinks=False):
        root_path = Path(root)
        relative_root = Path(".") if root_path == source else root_path.relative_to(source)
        kept_directories = []
        for name in directories:
            path = root_path / name
            relative = relative_root / name
            if checked_kind(path) != "directory":
                fail(f"Expected directory: {path}")
            if allow_runtime and relative == RUNTIME_DIRECTORY:
                continue
            actual_directories.add(relative)
            kept_directories.append(name)
        directories[:] = kept_directories

        for name in files:
            path = root_path / name
            relative = relative_root / name
            if checked_kind(path) != "file":
                fail(f"Expected regular file: {path}")
            if allow_runtime and relative in RUNTIME_FILES:
                continue
            actual_files.add(relative)

    if actual_directories != expected_directories:
        fail(
            "Unexpected release directories: "
            f"{sorted(map(str, actual_directories ^ expected_directories))}"
        )
    if actual_files != expected_files:
        fail(
            "Unexpected release files: "
            f"{sorted(map(str, actual_files ^ expected_files))}"
        )
    return source


def validate_preserved_data(path):
    path = Path(path)
    if checked_kind(path) != "directory":
        fail(f"Preserved data is not a directory: {path}")
    for root, directories, files in os.walk(path, topdown=True, followlinks=False):
        root_path = Path(root)
        for name in directories:
            child = root_path / name
            if checked_kind(child) != "directory":
                fail(f"Expected preserved-data directory: {child}")
        for name in files:
            child = root_path / name
            if checked_kind(child) != "file":
                fail(f"Expected preserved-data file: {child}")
    return path


def stage_release(source, destination):
    source = validate_source(source, allow_runtime=True)
    destination = Path(destination)
    if os.path.lexists(destination):
        fail(f"Stage destination already exists: {destination}")
    for relative, mode in DIRECTORIES:
        directory = destination if relative == Path(".") else destination / relative
        directory.mkdir(parents=True, exist_ok=False)
        directory.chmod(mode)
    for relative, mode in FILES:
        target = destination / relative
        shutil.copyfile(source / relative, target, follow_symlinks=False)
        target.chmod(mode)


def archive_info(name, mode, is_directory):
    info = zipfile.ZipInfo(name, ARCHIVE_TIME)
    info.create_system = 3
    file_type = stat.S_IFDIR if is_directory else stat.S_IFREG
    info.external_attr = (file_type | mode) << 16
    if is_directory:
        info.external_attr |= 0x10
        info.compress_type = zipfile.ZIP_STORED
    else:
        info.compress_type = zipfile.ZIP_DEFLATED
    return info


def package_release(source, archive):
    source = validate_source(source, allow_runtime=False)
    archive = Path(archive)
    archive.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{archive.name}.", dir=archive.parent
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with zipfile.ZipFile(
            temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
        ) as package:
            for relative, mode in DIRECTORIES:
                path = APP_ROOT if relative == Path(".") else APP_ROOT / relative
                package.writestr(archive_info(f"{path.as_posix()}/", mode, True), b"")
            for relative, mode in FILES:
                path = APP_ROOT / relative
                package.writestr(
                    archive_info(path.as_posix(), mode, False),
                    (source / relative).read_bytes(),
                )
        with zipfile.ZipFile(temporary) as package:
            expected = [
                f"{(APP_ROOT if path == Path('.') else APP_ROOT / path).as_posix()}/"
                for path, _ in DIRECTORIES
            ] + [(APP_ROOT / path).as_posix() for path, _ in FILES]
            if package.namelist() != expected or package.testzip() is not None:
                fail("Generated archive failed manifest or CRC validation")
        temporary.chmod(0o644)
        os.replace(temporary, archive)
    finally:
        if temporary.exists():
            temporary.unlink()


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    package = subparsers.add_parser("package")
    package.add_argument("source")
    package.add_argument("archive")
    stage = subparsers.add_parser("stage")
    stage.add_argument("source")
    stage.add_argument("destination")
    validate_data = subparsers.add_parser("validate-data")
    validate_data.add_argument("path")
    arguments = parser.parse_args()

    try:
        if arguments.command == "package":
            package_release(arguments.source, arguments.archive)
        elif arguments.command == "stage":
            stage_release(arguments.source, arguments.destination)
        else:
            validate_preserved_data(arguments.path)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"release tree error: {error}\n")


if __name__ == "__main__":
    main()

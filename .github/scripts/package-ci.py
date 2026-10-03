#!/usr/bin/env python3
"""Package the plugin using CMake's actual output paths, failing on missing inputs."""

import argparse
import json
import shutil
from pathlib import Path


def package(build_dir, config, platform, arch, output):
    source = Path(__file__).resolve().parents[2]
    artifact = json.loads((build_dir / f"artifact-{config}.json").read_text())
    binary = Path(artifact["binary"])
    if not binary.is_file():
        raise FileNotFoundError(binary)
    output.mkdir(parents=True, exist_ok=True)
    payload = output / "package"
    payload.mkdir()
    if platform == "macos":
        bundle = Path(artifact["bundle"])
        shutil.copytree(bundle, payload / bundle.name, symlinks=True)
    else:
        if platform == "windows":
            binaries = payload / "obs-plugins" / "64bit"
            data = payload / "data" / "obs-plugins" / "obs-shaderfilter"
        else:
            binaries = payload / "lib" / "x86_64-linux-gnu" / "obs-plugins"
            data = payload / "share" / "obs" / "obs-plugins" / "obs-shaderfilter"
        binaries.mkdir(parents=True)
        shutil.copy2(binary, binaries / binary.name)
        shutil.copytree(source / "data", data)
    archive = output / f"obs-shaderfilter-{platform}-{arch}"
    # Tar preserves macOS bundle permissions and symlinks.
    shutil.make_archive(str(archive), "zip" if platform == "windows" else "gztar", root_dir=payload)
    if platform == "windows":
        shutil.copy2(build_dir / "installer.iss", output / "installer.iss")
        shutil.copytree(source / "media", output / "media")
    return archive


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--platform", choices=("windows", "linux", "macos"), required=True)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(package(args.build_dir, args.config, args.platform, args.arch, args.output))

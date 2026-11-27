"""Build and package the public mod payload from a Git version tag."""

import argparse
import hashlib
import io
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile


def build_release(tag: str, output: Path | None) -> Path:
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag):
        raise ValueError("Expected a release tag like v0.1.0")

    root = Path(__file__).resolve().parent
    reference = f"refs/tags/{tag}^{{commit}}"
    commit = subprocess.check_output(["git", "rev-parse", "--verify", reference], cwd=root, text=True).strip()
    snapshot = subprocess.check_output(["git", "archive", "--format=zip", commit], cwd=root)
    output = (output or root / "dist").resolve()
    output.mkdir(parents=True, exist_ok=True)
    stem = f"HP2Mod-{tag}-win32"

    with tempfile.TemporaryDirectory(prefix="hp2-release-") as directory:
        project = Path(directory)
        with zipfile.ZipFile(io.BytesIO(snapshot)) as source:
            source.extractall(project)
        plugin = project / "out" / "HP2Display.asi"
        subprocess.run(
            ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
             str(project / "HP2Display" / "build.ps1"), "-OutputPath", str(plugin)],
            cwd=project, check=True,
        )

        payload = {
            "scripts/HP2Display.asi": plugin.read_bytes(),
            "scripts/HP2Display.ini": (project / "config/HP2Display.ini").read_bytes(),
            "patch_widescreen.py": (project / "HP2Display/patch_widescreen.py").read_bytes(),
            "THIRD_PARTY.md": (project / "THIRD_PARTY.md").read_bytes(),
            "README.md": (project / "README.md").read_bytes(),
            "CHANGELOG.md": (project / "CHANGELOG.md").read_bytes(),
            "AGENTS.md": (project / "AGENTS.md").read_bytes(),
            "BUILD.md": (project / "BUILD.md").read_bytes(),
            "docs/ENGINE.md": (project / "docs/ENGINE.md").read_bytes(),
            "docs/INSTALL.md": (project / "docs/INSTALL.md").read_bytes(),
            "docs/HP2WSFix.md": (project / "docs/HP2WSFix.md").read_bytes(),
            "docs/TESTING.md": (project / "docs/TESTING.md").read_bytes(),
        }
        hashes = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in payload.items())
        metadata = f"tag={tag}\ncommit={commit}\n\n{hashes}"
        with tempfile.TemporaryDirectory(prefix=".hp2-package-", dir=output) as staging:
            candidate = Path(staging) / f"{stem}.zip"
            with zipfile.ZipFile(candidate, "w", zipfile.ZIP_DEFLATED) as archive:
                for name, data in payload.items():
                    archive.writestr(name, data)
                archive.writestr("BUILD-INFO.txt", metadata)
            digest = hashlib.sha256(candidate.read_bytes()).hexdigest()
            checksum = Path(staging) / f"{stem}.zip.sha256"
            checksum.write_text(f"{digest}  {candidate.name}\n", encoding="ascii")
            final_zip = output / candidate.name
            candidate.replace(final_zip)
            checksum.replace(output / checksum.name)
    return final_zip


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("tag")
    cli.add_argument("--output", type=Path)
    args = cli.parse_args()
    print(build_release(args.tag, args.output))

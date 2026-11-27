"""Fix resolution validation and HUD score placement in the bundled HP2WSFix."""

from argparse import ArgumentParser
from hashlib import sha256
from os import replace
from pathlib import Path
from tempfile import NamedTemporaryFile


ORIGINAL_SHA256 = "935c17c45d79f94ae5c14d4cd353c8ab80f050a60cf7d0debb538ec8cc81252c"
RESOLUTION_PATCHES = (
    # CMP width, 0: jump to the desktop fallback if width <= 0.
    (0x68E5, bytes.fromhex("75 0e"), bytes.fromhex("7e 09")),
    # CMP height, 0: keep the profile only if height > 0.
    (0x68EE, bytes.fromhex("75 05"), bytes.fromhex("7f 05")),
)
PATCHES = RESOLUTION_PATCHES + (
    # score.HudText: move its left edge inside the screen in 800x600 HUD units.
    (0x13C0, bytes.fromhex("c7 46 08 00 00 00 00"), bytes.fromhex("c7 46 08 12 00 00 00")),
)


def fix_widescreen(original: bytes) -> bytes:
    if sha256(original).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("Unsupported HP2WSFix.asi build")
    fixed = bytearray(original)
    for offset, expected, changed in PATCHES:
        end = offset + len(expected)
        if fixed[offset:end] != expected:
            raise ValueError(f"Unexpected instruction at file offset {offset:#x}")
        fixed[offset:end] = changed
    return bytes(fixed)


def apply_fix(path: Path) -> None:
    current = path.read_bytes()
    backup = path.with_suffix(".asi.original")
    original = backup.read_bytes() if backup.exists() else current
    fixed = fix_widescreen(original)
    if current == fixed:
        print(f"Widescreen fixes already applied to {path}")
        return
    # Also accept the build with just the previous resolution fix applied.
    previous = bytearray(original)
    for offset, _, changed in RESOLUTION_PATCHES:
        previous[offset : offset + len(changed)] = changed
    if current not in (original, previous):
        raise ValueError("Unsupported HP2WSFix.asi changes")
    if not backup.exists():
        with backup.open("xb") as stream:
            stream.write(original)
    with NamedTemporaryFile(dir=path.parent, suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(fixed)
    try:
        replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)
    print(f"Patched {path}; original saved as {backup.name}")


if __name__ == "__main__":
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    apply_fix(parser.parse_args().path)

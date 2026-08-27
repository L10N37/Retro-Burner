#!/usr/bin/env python3

from pathlib import Path
import hashlib
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
WINDOWS_MAIN = ROOT / "src" / "main.cpp"
OUTPUT = ROOT / "src" / "draw_app_linux.generated.inl"


def extract_cpp_function(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise RuntimeError(f"Function signature not found: {signature}")

    brace = source.find("{", start)
    if brace < 0:
        raise RuntimeError(f"Opening brace not found: {signature}")

    depth = 0
    i = brace
    state = "code"
    quote = ""
    escape = False

    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""

        if state == "line_comment":
            if ch == "\n":
                state = "code"
            i += 1
            continue

        if state == "block_comment":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 2
                continue
            i += 1
            continue

        if state == "string":
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == quote:
                state = "code"
            i += 1
            continue

        if ch == "/" and nxt == "/":
            state = "line_comment"
            i += 2
            continue

        if ch == "/" and nxt == "*":
            state = "block_comment"
            i += 2
            continue

        if ch in ('"', "'"):
            state = "string"
            quote = ch
            escape = False
            i += 1
            continue

        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1] + "\n"

        i += 1

    raise RuntimeError(f"Unbalanced function: {signature}")


source = WINDOWS_MAIN.read_text(encoding="utf-8")
windows_draw_app = extract_cpp_function(source, "void DrawApp(")
source_sha = hashlib.sha256(windows_draw_app.encode("utf-8")).hexdigest()

linux_draw_app = windows_draw_app


# [RB-LINUX-TEXTURE-TRANSLATION]
# Windows DrawApp uses the D3D-backed `Texture` C++ identifier in the
# function signature and in local references such as `const Texture& artwork`.
# Linux renders the exact same DrawApp through the OpenGL-backed LinuxTexture.
#
# A whole-word regex is deliberate:
#   Texture       -> LinuxTexture
#   LinuxTexture  -> unchanged
#   ImTextureID   -> unchanged
linux_draw_app = re.sub(
    r"\bTexture\b",
    "LinuxTexture",
    linux_draw_app,
)

# D3D texture handle -> OpenGL ImGui texture ID.
linux_draw_app = linux_draw_app.replace(
    "reinterpret_cast<ImTextureID>(artwork.view.Get())",
    "LinuxTextureId(artwork)",
)

# Windows stores a two-character drive root (K:). Linux OpticalDrive::rootPath
# already contains the exact /dev/srX required by dvd+rw-tools.
root_assignment = re.compile(
    r"request\.opticalDriveRoot\s*=\s*"
    r"drive->rootPath\.size\(\)\s*>=\s*2\s*\?\s*"
    r"drive->rootPath\.substr\(0,\s*2\)\s*:\s*"
    r"\(drive->devicePath\.size\(\)\s*>=\s*6\s*\?\s*"
    r"drive->devicePath\.substr\(4,\s*2\)\s*:\s*"
    r"drive->rootPath\)\s*;",
    re.S,
)
linux_draw_app, root_count = root_assignment.subn(
    "request.opticalDriveRoot = drive->rootPath;",
    linux_draw_app,
)

standalone_root = re.compile(
    r"std::wstring\s+opticalDriveRoot\s*=\s*"
    r"drive->rootPath\.size\(\)\s*>=\s*2\s*\?\s*"
    r"drive->rootPath\.substr\(0,\s*2\)\s*:\s*"
    r"\(drive->devicePath\.size\(\)\s*>=\s*6\s*\?\s*"
    r"drive->devicePath\.substr\(4,\s*2\)\s*:\s*"
    r"drive->rootPath\)\s*;",
    re.S,
)
linux_draw_app, standalone_count = standalone_root.subn(
    "std::wstring opticalDriveRoot = drive->rootPath;",
    linux_draw_app,
)

# The UI is otherwise kept literal. Only platform-specific diagnostic wording
# is changed so Linux never tells the user a Windows drive cannot be accessed.
linux_draw_app = linux_draw_app.replace(
    "Could not access the selected Windows DVD writer for growisofs. Press Refresh.",
    "Could not access the selected Linux DVD writer for growisofs. Press Refresh.",
)

# Parity generator must never leave the Windows D3D texture wrapper in the
# Linux DrawApp. This catches future additions to the Windows UI automatically.
if re.search(r"\bTexture\b", linux_draw_app):
    leftovers = sorted(set(
        line.strip()
        for line in linux_draw_app.splitlines()
        if re.search(r"\bTexture\b", line)
    ))
    raise RuntimeError(
        "Generated Linux DrawApp still contains Windows Texture references:\n  "
        + "\n  ".join(leftovers)
    )

required_windows_strings = [
    "RETRO BURNER",
    "TARGET CONSOLE",
    "DISC IMAGE",
    "OPTICAL BURNER",
    "RECORDING BACKEND",
    "RetroBeam (default)",
    "growisofs (DVD profiles only)",
    "WRITE SPEED",
    "Automatic (drive/media)",
    " - Recommended",
    "Insert blank CD-R",
    "Insert blank DVD-R / DVD+R / DVD-DL",
    "Insert blank DVD+R DL",
    "ADVANCED SETTINGS",
    "Reset advanced defaults",
    "BURN-Free",
    "Force speed",
    "OPC POLICY",
    "MMC STREAMING / SPEED CONTROL",
    "DRIVE BUFFER",
    "XGD3 READY: successful preflight/preparation copy is cached.",
    "RECOMMENDED: run FULL XGD3 PREFLIGHT before a real burn.",
    "XGD3 PREPARATION",
    "TEST / ENABLE BURNERMAX",
    "Select a drive with a DVD+R DL inserted to test BurnerMAX.",
    "No disc sectors are written. The payload is verified by layer boundary and expanded writable capacity.",
    "BURN XBOX 360",
    "RECOMMENDED: FULL XGD3 PREFLIGHT - NO DISC WRITE",
    "Full preflight: ABGX360 AutoFix + verification, DVD+R DL/BurnerMAX checks and growisofs dry run.",
    "Full preflight: ABGX360 AutoFix + verification, DVD+R DL/BurnerMAX checks and RetroBeam no-write preflight.",
    "CDIrip / RetroBeam / ABGX360 / BurnerMAX output will appear here.",
    "Confirm burn",
    "Burn Xbox 360 now",
]

missing_from_windows = [
    text for text in required_windows_strings if text not in windows_draw_app
]
if missing_from_windows:
    raise RuntimeError(
        "Windows DrawApp no longer contains expected parity strings:\n  "
        + "\n  ".join(missing_from_windows)
    )

missing_from_linux = [
    text for text in required_windows_strings if text not in linux_draw_app
]
if missing_from_linux:
    raise RuntimeError(
        "Generated Linux DrawApp lost Windows UI strings:\n  "
        + "\n  ".join(missing_from_linux)
    )

header = (
    "// AUTO-GENERATED. DO NOT EDIT BY HAND.\n"
    "// Source of truth: src/main.cpp :: DrawApp()\n"
    f"// Windows DrawApp SHA-256: {source_sha}\n"
    f"// Linux platform substitutions: optical roots={root_count}, "
    f"BurnerMAX roots={standalone_count}, D3D texture handle -> OpenGL ID.\n\n"
)

OUTPUT.write_text(header + linux_draw_app, encoding="utf-8")

print(f"[UI SYNC] Windows DrawApp SHA-256: {source_sha}")
print(f"[UI SYNC] Generated: {OUTPUT.relative_to(ROOT)}")
print(f"[UI SYNC] request.opticalDriveRoot substitutions: {root_count}")
print(f"[UI SYNC] standalone BurnerMAX root substitutions: {standalone_count}")
print("[UI SYNC] Required Windows UI strings: PASS")

"""Windows half of Scripts/check_dependencies.py: check, optionally install, and validate what
KiEMS needs on Windows.

KiEMS builds on Windows with MSVC and vcpkg. The tools come from winget; every library comes
from vcpkg in manifest mode, from the root vcpkg.json, into out/vcpkg_installed (set by the
windows-* CMake presets). KiCad isn't built: libkicad's stock backend (the Windows default) reads
boards through an installed KiCad -- its Python with the pcbnew module, and kicad-cli -- so an
ordinary KiCad installation is required at run time.

By default it reports what is missing and prints the commands that install it, initializes the
submodules, and validates the result by configuring CMake. It installs tools only with --install.
Building needs a Visual Studio x64 Developer prompt (cl.exe on PATH) and VCPKG_ROOT set, both of
which Visual Studio's "x64 Native Tools Command Prompt" provides.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIGURE_PRESET = "windows-msvc-relwithdebinfo"
KICAD_WINGET_ID = "KiCad.KiCad"

# (what, winget id, extra winget arguments) -- components map onto these like the Linux manifest.
BUILD_TOOLS = {
    "cmake": ("CMake 3.25 or newer", "Kitware.CMake", []),
    "ninja": ("Ninja", "Ninja-build.Ninja", []),
    # VS 2026 is what KiCad's own Windows releases are built with; VS 2022 17.6 or newer works too.
    "cl": ("Visual Studio 2026 (or 2022) C++ build tools (MSVC 19.36 or newer) and a Windows SDK",
           "Microsoft.VisualStudio.BuildTools",
           ["--override", "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools "
                          "--includeRecommended"]),
}
RUNTIME_TOOLS = {
    "gnuplot": ("gnuplot (Matplot++ draws every plot through it; it must be on PATH)",
                "gnuplot.gnuplot", []),
}


def confirm(question: str, assume_yes: bool) -> bool:
    if assume_yes:
        return True
    try:
        return input(f"{question} [y/N] ").strip().lower() in {"y", "yes"}
    except EOFError:
        return False


def git_config(key: str) -> str:
    result = subprocess.run(["git", "-C", str(ROOT), "config", "--get", key], capture_output=True, text=True)
    return result.stdout.strip()


def msvc_version() -> tuple[int, int] | None:
    """cl.exe's own version, e.g. (19, 44), from its banner (it prints that to stderr)."""
    if not shutil.which("cl"):
        return None
    banner = subprocess.run(["cl"], capture_output=True, text=True).stderr
    match = re.search(r"Version (\d+)\.(\d+)", banner)
    return (int(match.group(1)), int(match.group(2))) if match else None


def visual_studio_with_cpp_tools() -> str | None:
    """Installation path of a Visual Studio (any edition, 2022 or later) that has the MSVC x64
    tools, even when they aren't on PATH. The installer's vswhere.exe exists as soon as the Visual
    Studio Installer does, with or without any C++ tools, so ask it rather than test for it."""
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                   "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not vswhere.is_file():
        return None
    result = subprocess.run([str(vswhere), "-latest", "-products", "*", "-version", "[17.6,",
                             "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                             "-property", "installationPath"], capture_output=True, text=True)
    return result.stdout.strip() or None


def check_tools(requested: list[str]) -> tuple[list[str], list[tuple[str, str, list[str]]]]:
    problems: list[str] = []
    installable: list[tuple[str, str, list[str]]] = []
    wanted = dict(BUILD_TOOLS) if "build" in requested else {}
    if "kiems" in requested:
        wanted.update(RUNTIME_TOOLS)
    for tool, (description, winget_id, extra) in wanted.items():
        if shutil.which(tool):
            print(f"  ok       {tool}")
            continue
        if tool == "gnuplot" and Path(os.environ.get("ProgramFiles", r"C:\Program Files"),
                                      "gnuplot", "bin", "gnuplot.exe").is_file():
            # Its installer leaves PATH alone; the kiems CLI looks here itself.
            print("  ok       gnuplot (in its standard location, not on PATH; kiems finds it there)")
            continue
        if tool == "cl" and (installation := visual_studio_with_cpp_tools()):
            problems.append("cl.exe is not on PATH: run this from Visual Studio's 'x64 Native Tools Command "
                            f"Prompt' (C++ tools are installed in {installation})")
            print("  missing  cl  (Visual Studio is installed; open its x64 Native Tools Command Prompt)")
            continue
        print(f"  missing  {tool}  ({description})")
        installable.append((tool, winget_id, extra))
    version = msvc_version()
    if version is not None and version < (19, 36):
        problems.append(f"MSVC {version[0]}.{version[1]} is too old; KiEMS needs 19.36 (VS 2022 17.6) or newer, "
                        "such as VS 2026")

    if "build" in requested:
        vcpkg_root = os.environ.get("VCPKG_ROOT")
        if not vcpkg_root or not (Path(vcpkg_root) / "scripts" / "buildsystems" / "vcpkg.cmake").is_file():
            message = "vcpkg is not set up: point VCPKG_ROOT at a vcpkg installation.\n"
            installation = visual_studio_with_cpp_tools()
            bundled = Path(installation, "VC", "vcpkg") if installation else None
            if bundled and (bundled / "scripts" / "buildsystems" / "vcpkg.cmake").is_file():
                message += (f"    Visual Studio includes one:\n"
                            f"      setx VCPKG_ROOT \"{bundled}\"\n"
                            "    or use a standalone clone at a short path:\n")
            else:
                message += "    Install it at a short path:\n"
            message += ("      git clone https://github.com/microsoft/vcpkg C:\\vcpkg\n"
                        "      C:\\vcpkg\\bootstrap-vcpkg.bat -disableMetrics\n"
                        "      setx VCPKG_ROOT C:\\vcpkg")
            problems.append(message)
            print("  missing  vcpkg  (VCPKG_ROOT)")
        else:
            print(f"  ok       vcpkg ({vcpkg_root})")
    return problems, installable


def check_checkout() -> list[str]:
    """Settings a Windows checkout of KiEMS and KiCad needs; reported, never changed."""
    warnings: list[str] = []
    if git_config("core.longpaths").lower() != "true":
        warnings.append("git core.longpaths is off; KiCad's deepest paths come close to Windows' 260-character "
                        "limit. Run: git config --global core.longpaths true")
    if len(str(ROOT)) > 40:
        warnings.append(f"the checkout path is {len(str(ROOT))} characters long; build trees nest deeply, so a "
                        "short path such as C:\\src\\KiEMS avoids MAX_PATH failures")
    return warnings


def kicad_python() -> Path | None:
    """The installed KiCad's Python, found the way libkicad's stock backend finds it
    (libkicad_stock.cpp): LIBKICAD_KICAD_PYTHON, else the newest C:\\Program Files\\KiCad\\<version>."""
    override = os.environ.get("LIBKICAD_KICAD_PYTHON")
    if override:
        return Path(override)
    root = Path(os.environ.get("ProgramFiles", r"C:\Program Files"), "KiCad")
    installs = []
    for directory in root.glob("*"):
        bin_dir = directory / "bin"
        if (bin_dir / "python.exe").is_file() and (bin_dir / "kicad-cli.exe").is_file():
            version = tuple(int(part) if part.isdigit() else 0 for part in directory.name.split("."))
            installs.append((version, bin_dir / "python.exe"))
    return max(installs)[1] if installs else None


def check_kicad() -> tuple[str | None, bool]:
    """(the KiCad version found, or None; whether it's usable). KiEMS reads boards through KiCad's
    own Python and its pcbnew module, so that import is the real test."""
    python = kicad_python()
    if python is None or not python.is_file():
        print("  missing  KiCad  (an installed KiCad; KiEMS reads boards through it)")
        return None, False
    probe = subprocess.run([str(python), "-c", "import pcbnew; print(pcbnew.GetBuildVersion())"],
                           capture_output=True, text=True)
    if probe.returncode != 0:
        print(f"  missing  KiCad's pcbnew Python module ({python} can't import it)")
        return None, False
    version = probe.stdout.strip().splitlines()[-1] if probe.stdout.strip() else "unknown"
    print(f"  ok       KiCad {version} ({python.parent})")
    return version, True


def update_submodules() -> int:
    command = ["git", "-C", str(ROOT), "submodule", "update", "--init", "--recursive"]
    print("\nInitializing Git submodules:")
    print("  " + subprocess.list2cmdline(command))
    result = subprocess.run(command)
    if result.returncode:
        print("Git could not initialize the submodules. If it reported 'Filename too long', run\n"
              "  git config --global core.longpaths true\nand rerun this checker.", file=sys.stderr)
    return result.returncode


def configure() -> int:
    command = ["cmake", "--preset", CONFIGURE_PRESET, "-B", str(ROOT / "out" / "windows-dependency-check"),
               "-DKIEMS_BUILD_TESTS=OFF"]
    print("\nValidating with CMake:")
    print("  " + subprocess.list2cmdline(command))
    return subprocess.run(command, cwd=ROOT).returncode


def main(args: argparse.Namespace, parser: argparse.ArgumentParser) -> int:
    if args.ssh:
        parser.error("--ssh only applies on macOS")
    requested = args.component or ["build", "kiems", "copper", "libkicad"]

    print("Windows build: MSVC + vcpkg (see the README's Windows section)\n\nTools:")
    problems, installable = check_tools(requested)
    warnings = check_checkout()
    if "libkicad" in requested or "kiems" in requested:
        _, usable = check_kicad()
        if not usable:
            installable.append(("KiCad", KICAD_WINGET_ID, []))

    if installable:
        commands = [["winget", "install", "--exact", "--id", winget_id, *extra] for _, winget_id, extra in installable]
        print("\nSuggested install commands:")
        for command in commands:
            print("  " + subprocess.list2cmdline(command))
        if args.install:
            if not shutil.which("winget"):
                print("winget is not available; install the tools above by hand.", file=sys.stderr)
                return 1
            if not confirm("Install these tools?", args.yes):
                print("Not installed.")
                return 1
            for command in commands:
                if args.yes:
                    command += ["--accept-package-agreements", "--accept-source-agreements"]
                if subprocess.run(command).returncode:
                    return 1
            print("\nInstalled. Open a new Visual Studio 'x64 Native Tools Command Prompt' so PATH picks the "
                  "tools up, then rerun this checker.")
            return 0

    for warning in warnings:
        print(f"\nwarning: {warning}")
    sys.stdout.flush()  # Keep the report in order: problems go to stderr.
    for problem in problems:
        print(f"\nerror: {problem}", file=sys.stderr)
    if installable or problems:
        return 1

    if update_submodules():
        return 1
    if args.no_configure:
        return 0
    return configure()

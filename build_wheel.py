#!/usr/bin/env python3
# build_wheel.py
import argparse
import contextlib
import json
import os
import platform
import re
import subprocess
import sys
from pathlib import Path

# --- Constants: Paths relative to the script's location ---
ROOT_DIR = Path(__file__).parent.resolve()
VCPKG_DIR = ROOT_DIR / "vcpkg"
VCPKG_JSON_PATH = ROOT_DIR / "vcpkg.json"
INIT_PY_PATH = ROOT_DIR / "src/python/cryptodd_arrays/__init__.py"
CMAKELIST_PATH = ROOT_DIR / "CMakeLists.txt"
PYPROJECT_TOML_PATH = ROOT_DIR / "pyproject.toml"
MIMALLOC_OVERLAY_DIR = ROOT_DIR / ".vcpkg-overlay-ports" / "mimalloc-empty"


# --- Context Manager for File Restoration ---

@contextlib.contextmanager
def file_modifier(filepath: Path):
    """A context manager to temporarily modify a file and restore it on exit."""
    original_content = None
    if filepath.exists():
        original_content = filepath.read_text(encoding="utf-8")

    try:
        yield
    finally:
        if original_content is not None:
            filepath.write_text(original_content, encoding="utf-8")
            print(f"Restored original content of {filepath.name}")


# --- Helper & System Detection Functions ---

def run_command(command, cwd=None, capture=False):
    """Executes a command and raises an exception on failure."""
    print(f"Running command: {' '.join(command)}")
    try:
        result = subprocess.run(
            command,
            cwd=cwd or ROOT_DIR,
            check=True,
            text=True,
            capture_output=capture,
        )
        if capture:
            return result.stdout.strip()
    except subprocess.CalledProcessError as e:
        print(f"Command failed with exit code {e.returncode}", file=sys.stderr)
        if e.stdout:
            print(f"STDOUT:\n{e.stdout}", file=sys.stderr)
        if e.stderr:
            print(f"STDERR:\n{e.stderr}", file=sys.stderr)
        sys.exit(e.returncode)
    except FileNotFoundError:
        print(f"Command not found: {command[0]}. Is it installed and in your PATH?", file=sys.stderr)
        sys.exit(1)

def detect_musl():
    """Detects if the system is using musl libc by checking for known musl loader paths."""
    if sys.platform != "linux":
        return None
    musl_loaders = ["/lib/ld-musl-x86_64.so.1", "/lib/ld-musl-aarch64.so.1", "/lib/ld-musl-armhf.so.1"]
    for loader in musl_loaders:
        if Path(loader).exists():
            print(f"Detected musl libc via {loader}")
            return True
    return False


# --- Environment Preparation ---

def check_linux_headers():
    """Checks for linux/perf_event.h and provides guidance if missing."""
    if sys.platform != "linux":
        return

    header_path = Path("/usr/include/linux/perf_event.h")
    if not header_path.exists():
        print("="*80, file=sys.stderr)
        print("ERROR: Linux kernel headers are missing.", file=sys.stderr)
        print(f"The required file '{header_path}' was not found.", file=sys.stderr)
        print("This is needed to compile the 'highway' dependency.", file=sys.stderr)
        print("\nPlease install the kernel headers for your distribution:", file=sys.stderr)
        print("  - On Alpine Linux: apk add linux-headers", file=sys.stderr)
        print("  - On Debian/Ubuntu:  sudo apt-get install linux-headers-$(uname -r)", file=sys.stderr)
        print("  - On Fedora/CentOS:  sudo dnf install kernel-headers", file=sys.stderr)
        print("="*80, file=sys.stderr)
        sys.exit(1)
    print("Linux kernel headers found.")

def create_static_linux_triplets():
    """Creates custom static triplets for Linux in the vcpkg community folder."""
    if sys.platform != "linux":
        return

    triplets_dir = VCPKG_DIR / "triplets" / "community"
    if not triplets_dir.is_dir():
        print(f"Warning: vcpkg community triplets directory not found at {triplets_dir}", file=sys.stderr)
        return

    static_triplets = {
        "x64-linux-static.cmake": """
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
""",
        "x86-linux-static.cmake": """
set(VCPKG_TARGET_ARCHITECTURE x86)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
""",
        "arm-linux-static.cmake": """
set(VCPKG_TARGET_ARCHITECTURE arm)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
""",
        "arm64-linux-static.cmake": """
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
"""
    }

    print("Ensuring static Linux triplets exist...")
    for filename, content in static_triplets.items():
        filepath = triplets_dir / filename
        if not filepath.exists():
            print(f"Creating static triplet: {filepath}")
            filepath.write_text(content.strip(), encoding="utf-8")
        else:
            print(f"Static triplet already exists: {filepath}")

# --- Git Workflow Functions ---
def check_git_dirty():
    print("Checking git repository status...")
    status = run_command(["git", "status", "--porcelain"], capture=True)
    if status:
        print("Error: Git repository is dirty. Please commit or stash your changes.", file=sys.stderr)
        print(status, file=sys.stderr)
        sys.exit(1)
    print("Git repository is clean.")

def init_submodules():
    print("Initializing git submodules...")
    if not (VCPKG_DIR / ".git").exists():
        run_command(["git", "submodule", "update", "--init", "--recursive"])
        print("Submodules initialized.")
    else:
        print("Submodules already initialized.")

def create_git_tag(version: str, tag_prefix: str = "v"):
    tag_name = f"{tag_prefix}{version}"
    print(f"Creating git tag: {tag_name}")
    try:
        subprocess.run(["git", "rev-parse", tag_name], check=True, capture_output=True, text=True)
        print(f"Warning: Tag '{tag_name}' already exists. Skipping tag creation.", file=sys.stderr)
        return
    except subprocess.CalledProcessError:
        pass
    run_command(["git", "tag", tag_name])
    print(f"Successfully created tag '{tag_name}'. Don't forget to 'git push --tags'.")

# --- Configuration and File Modification ---

def get_config(args: argparse.Namespace) -> dict:
    """Gathers all configuration from args and environment variables."""
    config = {}
    config['version'] = args.version or os.environ.get("CRYPTODD_VERSION")
    if not config['version']:
        with open(VCPKG_JSON_PATH, "r", encoding="utf-8") as f:
            config['version'] = json.load(f).get("version")
    if not config['version']:
        raise ValueError("Version could not be determined.")

    system = sys.platform
    machine = platform.machine().lower()

    if system == "win32":
        config['triplet'] = "x64-windows-static"
    elif system == "darwin":
        config['triplet'] = "arm64-osx" if machine == "arm64" else "x64-osx"
    elif system == "linux":
        arch_map = {"x86_64": "x64", "amd64": "x64", "aarch64": "arm64", "armv7l": "arm"}
        arch = arch_map.get(machine, machine)

        triplet = f"{arch}-linux-static"
        config['triplet'] = triplet
    else:
        raise NotImplementedError(f"Unsupported platform: {system}")

    mimalloc_setting = args.use_mimalloc or os.environ.get("CRYPTODD_USE_MIMALLOC")
    if not mimalloc_setting:
        mimalloc_setting = "off" if detect_musl() else "on"
    mimalloc_setting = mimalloc_setting.lower() in {"on", "true", "1", "yes", "y"}
    mimalloc_setting = "on" if mimalloc_setting else "off"
    config['use_mimalloc'] = mimalloc_setting
    return config

def manage_mimalloc_overlay(use_mimalloc: str) -> str | None:
    if use_mimalloc == "on":
        return None
    overlay_path_str = f"./{MIMALLOC_OVERLAY_DIR.relative_to(ROOT_DIR).as_posix()}"
    assert (ROOT_DIR / MIMALLOC_OVERLAY_DIR).is_dir(), "Mimalloc overlay directory not found"
    return overlay_path_str

def update_vcpkg_json(version: str, overlay_path: str | None):
    print(f"Updating {VCPKG_JSON_PATH} to version {version}...")
    with open(VCPKG_JSON_PATH, "r+", encoding="utf-8") as f:
        data = json.load(f)
        data["version"] = version
        if overlay_path:
            if "overlay-ports" not in data.get("vcpkg-configuration", {}):
                data.setdefault("vcpkg-configuration", {})["overlay-ports"] = []
            if overlay_path not in data["vcpkg-configuration"]["overlay-ports"]:
                data["vcpkg-configuration"]["overlay-ports"].append(overlay_path)
        f.seek(0)
        json.dump(data, f, indent=2)
        f.truncate()
        f.write("\n")

def update_init_py(version: str):
    content = INIT_PY_PATH.read_text(encoding="utf-8")
    content = re.sub(r'^__version__\s*=\s*".*"', f'__version__ = "{version}"', content, flags=re.MULTILINE)
    INIT_PY_PATH.write_text(content, encoding="utf-8")

def update_cmakelists(version: str):
    content = CMAKELIST_PATH.read_text(encoding="utf-8")
    content = re.sub(r"project\(\s*cryptodd_arrays[^\)]*\)", f"project(cryptodd_arrays VERSION {version} LANGUAGES CXX)", content)
    content = re.sub(r'VERSION_INFO\s*=\s*".*?"', 'VERSION_INFO="${PROJECT_VERSION}"', content)
    CMAKELIST_PATH.write_text(content, encoding="utf-8")

def update_pyproject_toml(triplet: str, use_mimalloc: str):
    content = PYPROJECT_TOML_PATH.read_text(encoding="utf-8")
    new_options = {"USE_MIMALLOC": f'"{use_mimalloc}"', "VCPKG_TARGET_TRIPLET": f'"{triplet}"'}
    if sys.platform == "win32":
        new_options["CMAKE_MSVC_RUNTIME_LIBRARY"] = '"MultiThreadedDLL"'
    options_str = ", ".join([f'{k}={v}' for k, v in new_options.items()])
    content = re.sub(r'^\s*options\s*=\s*{.*}', f'options = {{ {options_str} }}', content, flags=re.MULTILINE)
    if sys.platform != "win32":
        content = re.sub(r'^\s*build_type\s*=\s*".*?"', f'build_type = "Release" ', content, flags=re.MULTILINE)
    PYPROJECT_TOML_PATH.write_text(content, encoding="utf-8")

# --- Main Execution ---

def main():
    try:
        separator_index = sys.argv.index('--')
        script_args, build_args = sys.argv[1:separator_index], sys.argv[separator_index + 1:]
    except ValueError:
        script_args, build_args = sys.argv[1:], []

    parser = argparse.ArgumentParser(description="Build the cryptodd-arrays wheel.")
    parser.add_argument("--version", help="Override package version. Env: CRYPTODD_VERSION")
    parser.add_argument("--outdir", default="wheelhouse", help="Output directory for the built wheel.")
    parser.add_argument("--use-mimalloc", choices=['on', 'off'], help="Enable mimalloc. Env: CRYPTODD_USE_MIMALLOC")
    parser.add_argument("--prepare-only", action="store_true", help="Prepare files but do not build.")
    parser.add_argument("--no-cleanup", action="store_true", help="Do not restore modified files after build.")
    parser.add_argument("--create-tag", action="store_true", help="Create a git tag after a successful build.")
    parser.add_argument("--skip-git-checks", action="store_true", help="Skip checking for a clean git repo.")
    args = parser.parse_args(script_args)

    if not args.skip_git_checks:
        check_git_dirty()

    init_submodules()
    check_linux_headers()
    create_static_linux_triplets()

    config = get_config(args)
    mimalloc_overlay = manage_mimalloc_overlay(config['use_mimalloc'])

    print("-" * 50)
    print(f"Project: cryptodd-arrays, Version: {config['version']}")
    print(f"Platform Triplet: {config['triplet']}, Use Mimalloc: {config['use_mimalloc']}")
    print("-" * 50)

    files_to_manage = [PYPROJECT_TOML_PATH, CMAKELIST_PATH, INIT_PY_PATH, VCPKG_JSON_PATH]
    context_managers = [] if args.prepare_only or args.no_cleanup else [file_modifier(p) for p in files_to_manage]

    with contextlib.ExitStack() as stack:
        for cm in context_managers:
            stack.enter_context(cm)

        print("\n--- Modifying source files for build ---")
        update_pyproject_toml(config['triplet'], config['use_mimalloc'])
        update_vcpkg_json(config['version'], mimalloc_overlay)
        update_init_py(config['version'])
        update_cmakelists(config['version'])

        if args.prepare_only:
            print("\n--- Prepare Only Mode: Files modified. Skipping build. ---")
            sys.exit(0)

        print("\n--- Starting wheel build ---")
        Path(args.outdir).mkdir(exist_ok=True)
        build_command = [sys.executable, "-m", "pip", "wheel", "-v", "--wheel-dir", args.outdir, "."] + build_args
        run_command(build_command)

        print("\nBuild successful!")
        if args.create_tag:
            create_git_tag(config['version'])

if __name__ == "__main__":
    main()
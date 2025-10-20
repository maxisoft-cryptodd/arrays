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
VCPKG_JSON_PATH = ROOT_DIR / "vcpkg.json"
INIT_PY_PATH = ROOT_DIR / "src/python/cryptodd_arrays/__init__.py"
CMAKELIST_PATH = ROOT_DIR / "CMakeLists.txt"
PYPROJECT_TOML_PATH = ROOT_DIR / "pyproject.toml"


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


# --- Helper Functions ---

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


# --- Git Workflow Functions ---

def check_git_dirty():
    """Exits if the git repository has uncommitted changes."""
    print("Checking git repository status...")
    status = run_command(["git", "status", "--porcelain"], capture=True)
    if status:
        print("Error: Git repository is dirty. Please commit or stash your changes.", file=sys.stderr)
        print(status, file=sys.stderr)
        sys.exit(1)
    print("Git repository is clean.")

def init_submodules():
    """Initializes and updates git submodules."""
    print("Initializing git submodules...")
    # Check if vcpkg submodule dir exists and is empty
    vcpkg_dir = ROOT_DIR / "vcpkg"
    if not (vcpkg_dir / ".git").exists():
        run_command(["git", "submodule", "update", "--init", "--recursive"])
        print("Submodules initialized.")
    else:
        print("Submodules already initialized.")

def create_git_tag(version: str, tag_prefix: str = "v"):
    """Creates a git tag for the given version."""
    tag_name = f"{tag_prefix}{version}"
    print(f"Creating git tag: {tag_name}")

    # Check if tag already exists
    try:
        subprocess.run(["git", "rev-parse", tag_name], check=True, capture_output=True)
        print(f"Warning: Tag '{tag_name}' already exists. Skipping tag creation.", file=sys.stderr)
        return
    except subprocess.CalledProcessError:
        # Tag does not exist, which is what we want
        pass

    run_command(["git", "tag", tag_name])
    print(f"Successfully created tag '{tag_name}'. Don't forget to 'git push --tags'.")


# --- Configuration Functions ---

def get_config(args: argparse.Namespace) -> dict:
    """Gathers all configuration from args and environment variables."""
    config = {}

    # Version
    config['version'] = args.version or os.environ.get("CRYPTODD_VERSION")
    if not config['version']:
        with open(VCPKG_JSON_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
            config['version'] = data.get("version")
    if not config['version']:
        raise ValueError("Version could not be determined.")

    # VCPKG Triplet
    system = sys.platform
    machine = platform.machine().lower()
    if system == "win32":
        config['triplet'] = "x64-windows-static"
    elif system == "linux":
        config['triplet'] = "x64-linux"
    elif system == "darwin":  # macOS
        config['triplet'] = "arm64-osx" if machine == "arm64" else "x64-osx"
    else:
        raise NotImplementedError(f"Unsupported platform: {system}")

    # Mimalloc
    mimalloc_setting = args.use_mimalloc or os.environ.get("CRYPTODD_USE_MIMALLOC", "on")
    if mimalloc_setting.lower() not in ['on', 'off']:
        raise ValueError(f"Invalid USE_MIMALLOC value: {mimalloc_setting}. Must be 'on' or 'off'.")
    config['use_mimalloc'] = mimalloc_setting.lower()

    return config

# --- File Modification Functions ---

def update_vcpkg_json(version: str):
    """Updates the version in vcpkg.json."""
    print(f"Updating {VCPKG_JSON_PATH} to version {version}...")
    with open(VCPKG_JSON_PATH, "r+", encoding="utf-8") as f:
        data = json.load(f)
        data["version"] = version
        f.seek(0)
        json.dump(data, f, indent=2)
        f.truncate()
        f.write("\n")

def update_init_py(version: str):
    """Updates the __version__ in the Python package's __init__.py."""
    print(f"Updating {INIT_PY_PATH} to version {version}...")
    content = INIT_PY_PATH.read_text(encoding="utf-8")
    content = re.sub(r'^__version__\s*=\s*".*"', f'__version__ = "{version}"', content, flags=re.MULTILINE)
    INIT_PY_PATH.write_text(content, encoding="utf-8")

def update_cmakelists(version: str):
    """Updates the project version and version info macro in CMakeLists.txt."""
    print(f"Updating {CMAKELIST_PATH} to version {version}...")
    content = CMAKELIST_PATH.read_text(encoding="utf-8")
    content = re.sub(r"project\(\s*cryptodd_arrays\s*\)", f"project(cryptodd_arrays VERSION {version} LANGUAGES CXX)", content)
    content = re.sub(r"project\(\s*cryptodd_arrays\s*.*\s*\)", f"project(cryptodd_arrays VERSION {version} LANGUAGES CXX)", content)
    content = re.sub(r'VERSION_INFO\s*=\s*".*?"', 'VERSION_INFO="${PROJECT_VERSION}"', content)
    CMAKELIST_PATH.write_text(content, encoding="utf-8")

def update_pyproject_toml(triplet: str, use_mimalloc: str):
    """Updates VCPKG_TARGET_TRIPLET and other platform-specific options in pyproject.toml."""
    print(f"Updating {PYPROJECT_TOML_PATH} for triplet {triplet} (mimalloc: {use_mimalloc})...")
    content = PYPROJECT_TOML_PATH.read_text(encoding="utf-8")

    new_options = {
        "USE_MIMALLOC": f'"{use_mimalloc}"',
        "VCPKG_TARGET_TRIPLET": f'"{triplet}"'
    }
    if sys.platform == "win32":
        new_options["CMAKE_MSVC_RUNTIME_LIBRARY"] = '"MultiThreadedDLL"'

    options_str = ", ".join([f'{k}={v}' for k, v in new_options.items()])
    new_options_line = f'options = {{ {options_str} }}'

    content = re.sub(r'^\s*options\s*=\s*{.*}', new_options_line, content, flags=re.MULTILINE)
    PYPROJECT_TOML_PATH.write_text(content, encoding="utf-8")


# --- Main Execution ---

def main():
    """Main script execution."""
    # Separate script args from pass-through build args
    try:
        separator_index = sys.argv.index('--')
        script_args = sys.argv[1:separator_index]
        build_args = sys.argv[separator_index + 1:]
    except ValueError:
        script_args = sys.argv[1:]
        build_args = []

    parser = argparse.ArgumentParser(
        description="Build the cryptodd-arrays wheel with integrated versioning and git workflow.",
        epilog="Arguments after '--' are passed directly to the 'python -m build' command."
    )
    parser.add_argument("--version", help="Override package version (e.g., 1.2.3). Env: CRYPTODD_VERSION")
    parser.add_argument("--outdir", default="wheelhouse", help="Output directory for the built wheel.")
    parser.add_argument("--use-mimalloc", choices=['on', 'off'], help="Enable or disable mimalloc. Env: CRYPTODD_USE_MIMALLOC")
    parser.add_argument("--prepare-only", action="store_true", help="Modify files for the build but do not run the build or clean up.")
    parser.add_argument("--no-cleanup", action="store_true", help="Do not restore modified files after the build (for debugging).")
    parser.add_argument("--create-tag", action="store_true", help="Create a git tag after a successful build.")
    parser.add_argument("--skip-git-checks", action="store_true", help="Skip checking for a clean git repo.")
    args = parser.parse_args(script_args)

    # --- Workflow Start ---
    if not args.skip_git_checks:
        check_git_dirty()

    init_submodules()

    config = get_config(args)

    print("-" * 50)
    print(f"Project: cryptodd-arrays")
    print(f"Version: {config['version']}")
    print(f"Platform Triplet: {config['triplet']}")
    print(f"Use Mimalloc: {config['use_mimalloc']}")
    print(f"Output Directory: {args.outdir}")
    print("-" * 50)

    files_to_manage = [PYPROJECT_TOML_PATH, CMAKELIST_PATH, INIT_PY_PATH, VCPKG_JSON_PATH]

    context_managers = []
    if not args.prepare_only and not args.no_cleanup:
        context_managers = [file_modifier(p) for p in files_to_manage]

    with contextlib.ExitStack() as stack:
        for cm in context_managers:
            stack.enter_context(cm)

        # 1. Modify project files
        print("\n--- Modifying source files for build ---")
        update_pyproject_toml(config['triplet'], config['use_mimalloc'])
        update_vcpkg_json(config['version'])
        update_init_py(config['version'])
        update_cmakelists(config['version'])

        if args.prepare_only:
            print("\n--- Prepare Only Mode ---")
            print("Files have been modified for the build. Skipping build and cleanup.")
            sys.exit(0)

        # 2. Run the wheel build command
        print("\n--- Starting wheel build ---")
        Path(args.outdir).mkdir(exist_ok=True)
        build_command = [sys.executable, "-m", "pip", "wheel", "-v", "--wheel-dir", args.outdir, "."] + build_args

        run_command(build_command)

        print("\nBuild successful!")
        print(f"Wheel file created in '{args.outdir}' directory.")

        # 3. Post-build actions (e.g., tagging)
        if args.create_tag:
            print("\n--- Post-build actions ---")
            create_git_tag(config['version'])

if __name__ == "__main__":
    main()
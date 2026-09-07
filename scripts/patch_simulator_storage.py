"""Temporary consumer-carried simulator HAL patch; exact pinned images only."""
import hashlib
import json
from pathlib import Path
import subprocess


def apply_storage_patch(package, project):
    package, project = Path(package).resolve(), Path(project).resolve()
    manifest_path = project / "tools/simulator-patches/checked-storage.json"
    manifest = json.loads(manifest_path.read_text())
    revision = subprocess.check_output(["git", "-C", str(package), "rev-parse", "HEAD"], text=True).strip()
    if revision != manifest["revision"]:
        raise RuntimeError("Simulator storage patch: unrecognized dependency revision; preserve checkout and inspect")
    states = []
    for name, hashes in manifest["files"].items():
        path = package / name
        if path.is_symlink() or not path.is_file():
            raise RuntimeError(f"Simulator storage patch: missing or symlink preimage {path}")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        states.append("before" if digest == hashes["before"] else "after" if digest == hashes["after"] else "unknown")
    if all(state == "after" for state in states):
        return "already applied"
    if not all(state == "before" for state in states):
        raise RuntimeError("Simulator storage patch: unknown or mixed pre/post images; no files changed")
    patch = project / "tools/simulator-patches/checked-storage.patch"
    subprocess.run(["git", "-C", str(package), "apply", "--check", str(patch)], check=True)
    subprocess.run(["git", "-C", str(package), "apply", str(patch)], check=True)
    for name, hashes in manifest["files"].items():
        if hashlib.sha256((package / name).read_bytes()).hexdigest() != hashes["after"]:
            raise RuntimeError("Simulator storage patch: postimage verification failed")
    return "applied"


def configure(environment):
    import SCons.Script
    if "clean" in SCons.Script.COMMAND_LINE_TARGETS:
        return
    active = environment.subst("$PIOENV")
    if "simulator" not in active:
        return
    package = Path(environment.subst("$PROJECT_LIBDEPS_DIR")) / active / "simulator"
    if package.is_symlink() or not (package / "library.json").is_file():
        raise RuntimeError("Simulator storage patch: actual pinned dependency missing; custom symlink overrides unsupported")
    print("Simulator checked storage:", apply_storage_patch(package, environment.subst("$PROJECT_DIR")))


if "Import" in globals():
    Import("env")
    configure(env)

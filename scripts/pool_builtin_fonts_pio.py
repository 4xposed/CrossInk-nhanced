"""PlatformIO pre-script: generate font headers before dependency scanning."""
from pathlib import Path
import runpy

Import("env")

root = Path(env.subst("$PROJECT_DIR"))
output = Path(env.subst("$BUILD_DIR")) / "pooled-fonts"
generator = runpy.run_path(str(root / "scripts/pool_builtin_fonts.py"))
generator["generate"](root / "lib/EpdFont/builtinFonts", output)
# PlatformIO prepends library paths to CPPPATH after pre-scripts. Keep CPPPATH for
# dependency scanning, but also emit this -I in CCFLAGS: SCons places CCFLAGS
# before $_CPPINCFLAGS, so compiler lookup wins even after library discovery.
# The original EpdFont library stays in LDF.
env.Prepend(CPPPATH=[str(output)])
env.Prepend(CCFLAGS=["-I", str(output)])

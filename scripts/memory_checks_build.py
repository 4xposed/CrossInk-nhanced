"""Opt-in diagnostics only: normal firmware builds are unaffected."""
import os
import json
from pathlib import Path

Import('env')  # noqa: F821 - PlatformIO supplies the construction environment

if os.environ.get('CROSSINK_MEMORY_CHECKS') == '1':
    root = Path(env['PROJECT_DIR']).resolve()
    build_directory = Path(env.subst('$BUILD_DIR')).resolve()
    expected_reports = {}

    def write_stack_manifest(source, target, env):
        manifest = build_directory / 'memory-stack-manifest.json'
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(json.dumps(dict(sorted(expected_reports.items())), indent=2) + '\n')

    env.AddPostAction('buildprog', write_stack_manifest)
    # Refresh coverage even when the firmware itself is already up to date.
    env.AlwaysBuild(env.Alias('buildprog'))

    def memory_diagnostics(build_env, node):
        try:
            relative = Path(node.srcnode().get_abspath()).resolve().relative_to(root).as_posix()
        except ValueError:
            return node
        if not relative.startswith(('src/', 'lib/')) or relative.startswith(
            ('lib/uzlib/', 'lib/miniz/third_party/', 'lib/EpdFont/builtinFonts/',
             'lib/Epub/Epub/hyphenation/generated/')
        ) or node.get_suffix() not in ('.c', '.cpp', '.cc'):
            return node
        result = build_env.Object(node, CCFLAGS=build_env['CCFLAGS'] + [
            '-fstack-usage', '-Wstack-usage=512', '-Wframe-larger-than=512',
            '-Werror=alloca', '-Werror=vla',
        ])[0]  # Return one node so later ESP-IDF middlewares can inspect it.
        report = Path(result.get_abspath()).with_suffix('.su').relative_to(build_directory)
        expected_reports[report.as_posix()] = relative
        return result

    env.AddBuildMiddleware(memory_diagnostics)

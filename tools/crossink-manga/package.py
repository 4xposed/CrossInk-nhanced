"""CI-only packaging helper. Conversion itself is implemented in Rust."""
from pathlib import Path
import os
import shutil
import zipfile

root = Path(__file__).resolve().parent
suffix = '.exe' if os.name == 'nt' else ''
target_dir = Path(os.environ.get('CARGO_TARGET_DIR', root / 'target'))
binary = target_dir / 'release' / ('crossink-manga' + suffix)
helper = target_dir / 'release' / ('crossink-rar' + suffix)
models = Path(os.environ['NATIVE_MODELS_DIR'])
runtime = Path(os.environ['ORT_DYLIB_PATH'])
runtime_notices = Path(os.environ['ORT_LICENSE_DIR'])
required_models = ['comictextdetector.onnx', 'encoder.onnx', 'decoder_init.onnx', 'decoder.onnx', 'vocab.txt', 'recognizer.json', 'provenance.json', 'detector-LICENSE', 'recognizer-LICENSE']
for required in [binary, helper, runtime, runtime_notices / 'LICENSE'] + [models / name for name in required_models]:
    if not required.is_file():
        raise SystemExit(f'Missing native bundle input: {required}')

platform = os.environ['ARTIFACT_PLATFORM']
bundle = root / 'dist' / ('crossink-manga-' + platform)
bundle.mkdir(parents=True, exist_ok=False)
shutil.copy2(binary, bundle / binary.name)
shutil.copy2(helper, bundle / helper.name)
runtime_name = 'onnxruntime.dll' if os.name == 'nt' else ('libonnxruntime.dylib' if runtime.suffix == '.dylib' else 'libonnxruntime.so')
shutil.copy2(runtime, bundle / runtime_name)
# Provider libraries shipped beside ORT may be required by the native loader.
for dependency in runtime.parent.glob('*onnxruntime_providers*'):
    if dependency.is_file():
        shutil.copy2(dependency, bundle / dependency.name)
shutil.copytree(models, bundle / 'models')
shutil.copy2(root / 'LICENSE-MIT', bundle / 'LICENSE-MIT')
shutil.copy2(root / 'UPSTREAM-NOTICES.md', bundle / 'UPSTREAM-NOTICES.md')
shutil.copy2(root / 'README.md', bundle / 'README.md')
shutil.copy2(root / 'VALIDATION.md', bundle / 'VALIDATION.md')
shutil.copy2(root / 'LICENSE', bundle / 'LICENSE')
notices = bundle / 'licenses'
notices.mkdir()
for source in (root / 'third-party-licenses').iterdir():
    if source.is_file():
        shutil.copy2(source, notices / source.name)
for crate in sorted((root / 'vendor').iterdir()):
    if not crate.is_dir():
        continue
    for source in crate.rglob('*'):
        if source.is_file() and source.name.lower().startswith(('license', 'copying', 'notice')):
            target = notices / crate.name / source.relative_to(crate)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
for source in runtime_notices.iterdir():
    if source.is_file() and ('license' in source.name.lower() or 'notice' in source.name.lower()):
        shutil.copy2(source, notices / ('onnxruntime-' + source.name))
# GPL-derived native routines ship with corresponding source, including dependencies.
source_root = bundle / 'source' / 'crossink-manga'
shutil.copytree(root, source_root, ignore=shutil.ignore_patterns('target', 'dist', 'models-build', '__pycache__', '*.pyc', ':memory:.ses'))
config = source_root / '.cargo'
config.mkdir(exist_ok=True)
shutil.copy2(root / 'vendor-config.txt', config / 'config.toml')

archive = bundle.with_suffix('.zip')
# Cargo sources may use epoch timestamps, outside ZIP's representable range.
with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, strict_timestamps=False) as output:
    for source in sorted(bundle.rglob('*')):
        if source.is_file():
            output.write(source, Path(bundle.name) / source.relative_to(bundle))
print(archive)

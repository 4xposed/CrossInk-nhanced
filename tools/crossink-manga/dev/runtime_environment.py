"""CI-only: locate the installed CPU runtime without importing the model stack."""
import json
import os
from pathlib import Path
import onnxruntime

root = Path(onnxruntime.__file__).resolve().parent
binaries = [p for p in (root / 'capi').iterdir() if p.is_file() and p.name.startswith(('libonnxruntime.', 'onnxruntime.dll'))]
if len(binaries) != 1:
    raise SystemExit(f'Expected one runtime binary, found {binaries}')
values = {'ORT_DYLIB_PATH': str(binaries[0]), 'ORT_LICENSE_DIR': str(root)}
print(json.dumps(values, indent=2))
if os.environ.get('GITHUB_ENV'):
    with open(os.environ['GITHUB_ENV'], 'a') as output:
        for key, value in values.items():
            output.write(f'{key}={value}\n')

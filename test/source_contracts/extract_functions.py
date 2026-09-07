"""Extract exact production function definitions for isolated native boundary tests.

Only UI/platform collaborators are doubled; function bodies are never rewritten.
CMake depends on both the source and this script, so edits rebuild the harness.
"""
import pathlib
import re
import sys

def extract(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    for token in tokens.finditer(source, opening):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return source[start:token.end()] + '\n'
    raise ValueError('Unbalanced production function: ' + signature)

if __name__ == '__main__':
    output, *pairs = sys.argv[1:]
    result = []
    for source, signature in zip(pairs[::2], pairs[1::2]):
        result.append(extract(pathlib.Path(source).read_text(), signature))
    pathlib.Path(output).write_text('\n'.join(result))

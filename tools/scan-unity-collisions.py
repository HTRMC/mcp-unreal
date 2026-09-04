#!/usr/bin/env python3
"""Report anonymous-namespace helper names shared by two files in one module.

Adaptive unity builds merge whichever .cpp files were not edited recently into
one translation unit. Two files in the same module that each define, say, a
helper called `IntOr` in an anonymous namespace then collide -- and *which*
files get merged depends on file mtimes, so the failure appears and disappears
between builds and machines. Run this before pushing.

Only names declared directly inside an anonymous namespace are considered,
since those are the ones that collide. Overloads (same name, different
parameter types) are legal C++ and still show up here, so read the hits rather
than just counting them.

Usage: py -3 tools/scan-unity-collisions.py
"""
import collections
import glob
import os
import re
import sys

# A declaration at the immediate scope of an anonymous namespace: a return type
# (possibly templated) followed by an identifier and then `(` or `=`.
DECLARATION = re.compile(
    r'^(?:static\s+|constexpr\s+|const\s+|inline\s+)*'
    r'[A-Za-z_][\w:<>,\s\*&\[\]]*?\b(\w+)\s*(?:\(|=)')

KEYWORDS = {'if', 'for', 'while', 'return', 'switch', 'else', 'case', 'TEXT',
            'NSLOCTEXT', 'check', 'checkf', 'ensure', 'namespace'}


def anonymous_namespace_bodies(lines):
    """Yield (indent, body_lines) for each `namespace {` block with no name."""
    index = 0
    while index < len(lines):
        line = lines[index]
        stripped = line.strip()
        opens_here = stripped == 'namespace {'
        # The house style puts the brace on its own line.
        opens_next = (stripped == 'namespace'
                      and index + 1 < len(lines)
                      and lines[index + 1].strip() == '{')
        if not (opens_here or opens_next):
            index += 1
            continue

        indent = len(line) - len(line.lstrip())
        depth = 0
        body = []
        cursor = index if opens_here else index + 1
        while cursor < len(lines):
            depth += lines[cursor].count('{') - lines[cursor].count('}')
            if depth == 0:
                break
            body.append(lines[cursor])
            cursor += 1
        yield indent, body[1:]
        index = cursor + 1


def names_in(path):
    """Helper names declared at the top level of this file's anonymous namespaces."""
    with open(path, encoding='utf-8') as handle:
        lines = handle.read().splitlines()

    found = set()
    for indent, body in anonymous_namespace_bodies(lines):
        member_indent = indent + 1
        for line in body:
            if not line.strip() or line.lstrip().startswith('//'):
                continue
            if len(line) - len(line.lstrip()) != member_indent:
                continue
            match = DECLARATION.match(line.strip())
            if match and match.group(1) not in KEYWORDS:
                found.add(match.group(1))
    return found


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    pattern = os.path.join(root, 'plugin', '*', 'Source', '*', 'Private', '**', '*.cpp')

    modules = collections.defaultdict(lambda: collections.defaultdict(set))
    for path in glob.glob(pattern, recursive=True):
        parts = path.replace(os.sep, '/').split('/')
        module = parts[parts.index('Source') + 1]
        for name in names_in(path):
            modules[module][name].add(os.path.basename(path))

    collisions = [(module, name, sorted(files))
                  for module, names in sorted(modules.items())
                  for name, files in sorted(names.items())
                  if len(files) > 1]

    if not collisions:
        print('no shared helper names within a module')
        return 0
    for module, name, files in collisions:
        print(f'{module}: {name} in {", ".join(files)}')
    print(f'\n{len(collisions)} candidate(s) - promote genuinely shared helpers into '
          'McpLinkCore, or rename one copy. Overloads are fine.')
    return 1


if __name__ == '__main__':
    sys.exit(main())

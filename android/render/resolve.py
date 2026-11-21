#!/usr/bin/env python3
"""Resolves the conflict hunks of render/libs/video/<file> (produced by merge_file.py) by an explicit, reviewed list of decisions.
   usage in a script:  resolve(path, [decision, ...])  one decision per hunk, in file order:
     'ours' | 'theirs' | 'both' (fork block then upstream block) | ('both_sep', text) (fork block, text, upstream block) | ('text', t)
   Fails loudly if the number of decisions differs from the number of hunks, so a changed merge can never be resolved blind."""
import re
def resolve(path, decisions):
    s = open(path).read().split('\n'); out = []; i = 0; k = 0
    while i < len(s):
        if s[i].startswith('<<<<<<<'):
            i += 1; a = []; b = []
            while not s[i].startswith('======='): a.append(s[i]); i += 1
            i += 1
            while not s[i].startswith('>>>>>>>'): b.append(s[i]); i += 1
            i += 1
            if k >= len(decisions): raise SystemExit(f'{path}: more hunks than decisions')
            d = decisions[k]; k += 1
            if d == 'ours': out += a
            elif d == 'theirs': out += b
            elif d == 'both': out += a + b
            elif d[0] == 'both_sep': out += a + d[1].split('\n') + b
            elif d[0] == 'text': out += d[1].split('\n')
            else: raise SystemExit('bad decision')
        else: out.append(s[i]); i += 1
    if k != len(decisions): raise SystemExit(f'{path}: {len(decisions)} decisions for {k} hunks')
    open(path, 'w').write('\n'.join(out))
    return k

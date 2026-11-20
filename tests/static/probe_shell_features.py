#!/usr/bin/env python3
"""Compile real shell expansion/editor functions, exercise real host directories and deterministic keys."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/static'))
from rules import functions

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=ROOT)
    ap.add_argument('--mutate', choices=('none', 'variables', 'glob', 'history', 'completion', 'editor'), default='none')
    a = ap.parse_args()
    root = a.source.resolve()
    out = Path(tempfile.mkdtemp(prefix='welyos-shell-features-'))
    paths = ['user/sh.c', 'user/sh_words.h', 'user/sh_edit.h']
    missing = [p for p in paths if not (root / p).is_file()]
    if missing:
        print('FAIL: missing shell feature implementation:', ', '.join(missing))
        return 1
    data = {p: (root / p).read_bytes() for p in paths}
    src = data['user/sh.c'].decode()
    f = next(f for f in functions('user/sh.c', src) if f.name == 'parse')
    (out / 'parse.c').write_text(''.join(src.splitlines(keepends=True)[f.line-1:f.end]))
    funcs = {f.name: f for f in functions('user/sh.c', src)}
    for name, dest in (('read_line', 'reader.c'), ('main', 'shell_main.c')):
        f = funcs[name]
        body = ''.join(src.splitlines(keepends=True)[f.line-1:f.end])
        if name == 'main':
            body = body.replace('int main(', 'static int shell_main(', 1)
        (out / dest).write_text(body)
    word_src = data['user/sh_words.h'].decode()
    edit_src = data['user/sh_edit.h'].decode()
    if a.mutate == 'variables':
        word_src = word_src.replace('return sh_vars[i].value;', 'return "";')
    elif a.mutate == 'glob':
        word_src = word_src.replace('if (wild && !(sh_kind[w]', 'if (0 && wild && !(sh_kind[w]')
    elif a.mutate == 'history':
        edit_src = edit_src.replace('if (!*line ||', 'if (1 || !*line ||')
    elif a.mutate == 'completion':
        edit_src = edit_src.replace('if (!len) return 0;', 'if (1 || !len) return 0;')
    elif a.mutate == 'editor':
        edit_src = edit_src.replace("if (c == 'D' && pos) pos--;", "if (c == 'D' && pos) pos = pos;")
    (out / 'words.c').write_text(word_src)
    (out / 'edit.c').write_text(edit_src)
    for name in ('fs', 'proc', 'errno'):
        p = 'include/uapi/' + name + '.h'
        base = root / p if (root / p).exists() else ROOT / p
        data[p] = base.read_bytes()
        (out / (name + '.h')).write_bytes(data[p])
    (out / 'probe.c').write_bytes((ROOT / 'tests/host/shell/features_probe.c').read_bytes())
    (out / 'data/sub').mkdir(parents=True)
    (out / 'special').mkdir()
    for p in ('data/a.txt', 'data/b.txt', 'data/.hidden', 'data/sub/c.txt', 'special/a b&c'):
        (out / p).write_text('fixture\n')
    subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie',
                    str(out / 'probe.c'), '-o', str(out / 'probe')], check=True)
    rows = []
    for case in ('variables', 'quotes-operators', 'glob', 'glob-io', 'limits', 'editor', 'history', 'completion', 'script-main', 'script-overflow', 'fuzz-words'):
        p = subprocess.run([str(out / 'probe'), case], cwd=out, capture_output=True, text=True, timeout=10)
        rows.append(dict(case=case, returncode=p.returncode, stdout=p.stdout, stderr=p.stderr))
        print(p.stdout.strip(), p.stderr[:2000])
    hashes = {p: hashlib.sha256(b).hexdigest() for p, b in data.items()}
    changed = [p for p in paths if hashlib.sha256((root / p).read_bytes()).hexdigest() != hashes[p]]
    (out / 'results.json').write_text(json.dumps(dict(results=rows, mutation=a.mutate, source_hashes=hashes, changed_inputs=changed,
        model='real lexer/parser/glob/editor code; POSIX directories, deterministic keys; not a real console or spawned process'), indent=2))
    print('artifacts:', out)
    return 2 if changed else int(any(r['returncode'] for r in rows))
if __name__ == '__main__':
    raise SystemExit(main())

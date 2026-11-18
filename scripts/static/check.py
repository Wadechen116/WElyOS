#!/usr/bin/env python3
"""以來源快照執行檢查;只寫指定的報告目錄與 /tmp,絕不建置 kernel。"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from rules import check

ROOT = Path(__file__).resolve().parents[2]
TREES = ('kernel', 'arch', 'lib', 'user', 'include', 'cmake')
SCHEMA_VERSION = 2
SCAN_SCOPE = {'source_roots': ['kernel', 'arch', 'lib', 'user', 'include'],
              'loop_bound_roots': ['kernel/net/'],
              'recursion_targets': ['i386', 'riscv64', 'aarch64']}


def run(args, cwd=None, timeout=30):
    try:
        proc = subprocess.run(args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, errors='replace', timeout=timeout)
        return {'returncode': proc.returncode, 'stdout': proc.stdout, 'stderr': proc.stderr}
    except subprocess.TimeoutExpired as exc:
        return {'returncode': None, 'stdout': '', 'stderr': f'timeout after {timeout}s: {exc.cmd[0]}'}
    except OSError as exc:
        return {'returncode': None, 'stdout': '', 'stderr': str(exc)}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def snapshot(root, destination):
    hashes, texts = {}, {}
    paths = [root / 'CMakeLists.txt']
    for tree in TREES:
        paths.extend(p for p in sorted((root / tree).rglob('*')) if p.is_file())
    for path in paths:
        if path.is_symlink():
            raise ValueError(f'symlink not supported in source snapshot: {path}')
        rel = path.relative_to(root)
        data = path.read_bytes()
        hashes[str(rel)] = digest(data)
        dest = destination / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        if path.suffix in ('.c', '.h') and rel.parts[0] != 'cmake':
            texts[str(rel)] = data.decode('utf-8', errors='replace')
    return hashes, texts


def changed_inputs(hashes):
    changed = []
    for rel, sha in hashes.items():
        path = ROOT / rel
        if not path.is_file() or digest(path.read_bytes()) != sha:
            changed.append(rel)
    current = {'CMakeLists.txt'}
    for tree in TREES:
        current.update(str(p.relative_to(ROOT)) for p in (ROOT / tree).rglob('*') if p.is_file())
    return sorted(set(changed) | (current - hashes.keys()))


def load_baseline(path):
    # 每列都必須明寫審查者與理由;此工具不提供「把本次全部接受」的開關。
    result = {}
    if not path.exists():
        return result
    for line_no, text in enumerate(path.read_text().splitlines(), 1):
        if not text.strip() or text.lstrip().startswith('#'):
            continue
        item = json.loads(text)
        if not isinstance(item, dict) or not isinstance(item.get('id'), str) or not re.fullmatch(r'[0-9a-f]{24}', item['id']):
            raise ValueError(f'{path}:{line_no}: invalid finding id')
        for key in ('reviewer', 'reason', 'date'):
            if not str(item.get(key, '')).strip():
                raise ValueError(f'{path}:{line_no}: missing {key}')
        if item['id'] in result:
            raise ValueError(f'{path}:{line_no}: duplicate finding id')
        result[item['id']] = item
    return result


def load_comparison(path, git_reference, rules_fingerprint):
    """比較報告是歷史資料,不代表已審查;Git 來源固定到不可變的 commit。"""
    if git_reference:
        revision, separator, rel = git_reference.partition(':')
        if not separator or not revision or revision.startswith('-') or not rel or \
                Path(rel).is_absolute() or '..' in Path(rel).parts:
            raise ValueError('--compare-ref must be REV:repository/relative/report.json')
        commit = run(['git', 'rev-parse', '--verify', '--end-of-options', f'{revision}^{{commit}}'], cwd=ROOT)
        if commit['returncode'] != 0:
            raise ValueError('comparison Git commit unavailable')
        sha = commit['stdout'].strip()
        blob = run(['git', 'show', f'{sha}:{rel}'], cwd=ROOT)
        if blob['returncode'] != 0:
            raise ValueError('comparison report absent from selected Git commit')
        raw = blob['stdout']
        provenance = {'kind': 'git', 'commit': sha, 'path': rel, 'sha256': digest(raw.encode())}
    else:
        raw = path.read_text()
        provenance = {'kind': 'file', 'path': str(path.resolve()), 'sha256': digest(raw.encode())}
    reference = json.loads(raw)
    if not isinstance(reference, dict) or reference.get('schema_version') != SCHEMA_VERSION or \
            reference.get('mode') != 'fast' or reference.get('scan_scope') != SCAN_SCOPE or \
            reference.get('rules_fingerprint') != rules_fingerprint:
        raise ValueError('comparison schema/mode/scope/rules differ; create a compatible reference explicitly')
    if reference.get('changed_inputs') != [] or reference.get('changed_tools') != [] or \
            type(reference.get('exit_code')) is not int or reference['exit_code'] not in (0, 1):
        raise ValueError('comparison reference is stale, incomplete, or lacks a valid gate result')
    coverage = reference.get('coverage')
    if not isinstance(coverage, list) or not any(isinstance(c, dict) and c.get('tool') == 'lexical' and
            c.get('status') == 'heuristic' for c in coverage) or any(
            not isinstance(c, dict) or c.get('status') not in ('heuristic', 'not-requested') for c in coverage):
        raise ValueError('comparison reference coverage unavailable or invalid')
    findings = reference.get('findings')
    if not isinstance(findings, list):
        raise ValueError('comparison reference lacks findings')
    seen = set()
    for f in findings:
        if not isinstance(f, dict) or not isinstance(f.get('id'), str) or not \
                re.fullmatch(r'[0-9a-f]{24}', f['id']) or f['id'] in seen or \
                type(f.get('accepted')) is not bool or any(
                    not isinstance(f.get(key), str) for key in ('path', 'rule', 'category')):
            raise ValueError('comparison reference has malformed or duplicate findings')
        seen.add(f['id'])
    if reference.get('new_count') != sum(not f['accepted'] for f in findings) or \
            reference.get('accepted_count') != sum(f['accepted'] for f in findings):
        raise ValueError('comparison reference totals disagree with its findings')
    return reference, provenance


def compare_findings(findings, reference, provenance):
    old = {f['id']: f for f in reference['findings']}
    current = {f['id']: f for f in findings}
    old_pending = {key for key, f in old.items() if not f['accepted']}
    pending = {key for key, f in current.items() if not f['accepted']}
    return {'reference': provenance,
            'introduced_ids': sorted(current.keys() - old.keys()),
            'resolved_ids': sorted(old.keys() - current.keys()),
            'newly_unreviewed_ids': sorted(pending - old_pending),
            'existing_unreviewed_ids': sorted(pending & old_pending),
            'reopened_ids': sorted((pending & old.keys()) - old_pending)}


def diagnostic_id(tool, target, path, option, symbol, message, sha):
    text = '|'.join((tool, target, path, option, symbol, message, sha))
    return digest(text.encode())[:24]


def normalize(text, snap):
    return text.replace(str(snap), '<source>').replace(str(ROOT), '<source>')


def gcc_diagnostics(stderr, snap, target, hashes):
    items = json.loads(stderr or '[]')
    if not isinstance(items, list) or any(not isinstance(item, dict) for item in items):
        raise ValueError('invalid GCC JSON diagnostics')
    findings, exhausted = [], False
    for item in items:
        if item.get('kind') not in ('warning', 'error', 'fatal error'):
            continue
        option = item.get('option', item.get('kind', 'diagnostic'))
        exhausted |= option == '-Wanalyzer-too-complex'
        locations = item.get('locations', [])
        loc = locations[0].get('caret', {}) if locations else {}
        path = loc.get('file', '')
        try:
            path = str(Path(path).relative_to(snap))
        except ValueError:
            pass
        message = normalize(item.get('message', ''), snap)
        rule = 'gcc-complexity' if option == '-Wanalyzer-too-complex' else 'gcc-analyzer'
        findings.append({'id': diagnostic_id('gcc', target, path, option, '', message,
                                             hashes.get(path, 'external')),
                         'rule': rule, 'category': 'coverage' if rule == 'gcc-complexity' else 'candidate',
                         'path': path, 'line': loc.get('line', 0), 'symbol': '',
                         'message': f'{target} {option}: {message}', 'target': target,
                         'source_sha256': hashes.get(path), 'diagnostic': item})
    return findings, exhausted


def compilation_database(snap, out, target):
    commands, statuses = [], []
    for variant in ('ring', 'list'):
        build = out / f'configure-{target}-{variant}'
        args = ['cmake', '-S', str(snap), '-B', str(build), f'-DARCH={target}',
                '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', f'-DWELYOS_RUNQ={variant}']
        proc = run(args, timeout=45)
        (out / f'cmake-{target}-{variant}.log').write_text(proc['stdout'] + proc['stderr'])
        if proc['returncode'] != 0:
            statuses.append({'tool': 'cmake', 'target': target, 'status': 'unavailable',
                             'reason': f'configure failed for {variant}; see cmake log'})
            continue
        db = json.loads((build / 'compile_commands.json').read_text())
        for record in db:
            path = Path(record['file'])
            if path.suffix != '.c':
                continue
            args = record.get('arguments') or shlex.split(record['command'])
            # 只替換物件輸出路徑,其餘編譯旗標與 include/define 全部沿用 CMake。
            cleaned, i = [], 0
            while i < len(args):
                if args[i] == '-o':
                    i += 2
                else:
                    cleaned.append(args[i])
                    i += 1
            key = tuple(cleaned)
            if not any(tuple(c['arguments']) == key for c in commands):
                commands.append({'arguments': cleaned, 'directory': record['directory'],
                                 'file': str(path), 'variant': variant})
    (out / f'compile_commands-{target}.json').write_text(json.dumps(commands, indent=2))
    return commands, statuses


def analyze(snap, out, hashes, options):
    coverage, findings, command_log = [], [], []
    compilers = {'i386': 'gcc', 'riscv64': 'riscv64-unknown-elf-gcc',
                 'aarch64': 'aarch64-linux-gnu-gcc'}
    all_commands = []
    if not shutil.which('cmake'):
        return [], [{'tool': 'cmake', 'status': 'unavailable', 'reason': 'coverage unavailable: missing cmake'}]
    for target in options.targets:
        compiler = compilers[target]
        if not shutil.which(compiler):
            coverage.append({'tool': 'gcc', 'target': target, 'status': 'unavailable',
                             'reason': f'coverage unavailable: missing {compiler}'})
            continue
        version = run([compiler, '--version'])['stdout'].splitlines()[0]
        probe = out / f'probe-{target}.c'
        probe.write_text('int f(void) { return 0; }\n')
        proc = run([compiler, '-fanalyzer', '-fdiagnostics-format=json', '-c', str(probe),
                    '-o', str(out / f'probe-{target}.o')])
        if proc['returncode'] != 0:
            coverage.append({'tool': 'gcc', 'target': target, 'version': version, 'status': 'unavailable',
                             'reason': f'coverage unavailable: -fanalyzer unsupported: {proc["stderr"]}'})
            continue
        commands, statuses = compilation_database(snap, out, target)
        coverage.extend(statuses)
        for number, record in enumerate(commands):
            all_commands.append((target, number, record))
        coverage.append({'tool': 'gcc', 'target': target, 'version': version,
                         'status': 'pending', 'translation_units': len(commands)})
    start = time.monotonic()
    def one(entry):
        target, number, rec = entry
        if time.monotonic() - start >= options.budget:
            return entry, {'returncode': None, 'stdout': '', 'stderr': 'total analyzer budget exhausted'}, []
        args = rec['arguments'] + ['-fanalyzer', '-Wanalyzer-too-complex',
                '-fdiagnostics-format=json', '-fdiagnostics-color=never',
                '-o', str(out / f'analyzer-{target}-{number}.o')]
        proc = run(args, cwd=rec['directory'], timeout=options.timeout)
        return entry, proc, args
    completed = Counter()
    incomplete = set()
    with ThreadPoolExecutor(max_workers=options.jobs) as pool:
        futures = [pool.submit(one, entry) for entry in all_commands]
        for future in as_completed(futures):
            (target, number, rec), proc, args = future.result()
            (out / f'gcc-{target}-{number}.json').write_text(proc['stderr'])
            command_log.append({'target': target, 'file': rec['file'], 'arguments': args,
                                'returncode': proc['returncode']})
            try:
                fresh, exhausted = gcc_diagnostics(proc['stderr'], snap, target, hashes)
                findings.extend(fresh)
                if exhausted:
                    incomplete.add(target)
            except (ValueError, TypeError, KeyError):
                incomplete.add(target)
            if proc['returncode'] != 0:
                incomplete.add(target)
                coverage.append({'tool': 'gcc-tu', 'target': target, 'status': 'unavailable',
                                 'file': str(Path(rec['file']).relative_to(snap)),
                                 'reason': f'coverage unavailable: rc={proc["returncode"]}; see gcc-{target}-{number}.json'})
            completed[target] += 1
            if sum(completed.values()) % 20 == 0:
                print(f'GCC analyzer: {sum(completed.values())}/{len(all_commands)} units', flush=True)
    for status in coverage:
        if status['status'] == 'pending':
            status['completed_units'] = completed[status['target']]
            status['status'] = 'partial' if status['target'] in incomplete else 'complete'
            if status['status'] == 'partial':
                status['reason'] = 'coverage unavailable for some paths/units; inspect diagnostics and logs'
    (out / 'analyzer-commands.json').write_text(json.dumps(command_log, indent=2))
    # cppcheck 缺失不安裝、不假裝 pass。可用時沿用每個 target 的完整 compile database。
    if not shutil.which('cppcheck'):
        coverage.append({'tool': 'cppcheck', 'status': 'unavailable',
                         'reason': 'coverage unavailable: cppcheck not installed'})
    else:
        import xml.etree.ElementTree as ET
        for target in options.targets:
            database = out / f'compile_commands-{target}.json'
            if not database.exists():
                coverage.append({'tool': 'cppcheck', 'target': target, 'status': 'unavailable',
                                 'reason': 'coverage unavailable: no compile database'})
                continue
            platform = 'unix32' if target == 'i386' else 'unix64'
            proc = run(['cppcheck', f'--project={database}', f'--platform={platform}',
                        '--enable=warning,style,performance,portability',
                        '--xml', '--xml-version=2', '--inline-suppr', '--error-exitcode=1'], timeout=options.budget)
            (out / f'cppcheck-{target}.xml').write_text(proc['stderr'])
            try:
                root = ET.fromstring(proc['stderr'])
                for item in root.findall('./errors/error'):
                    loc = item.find('location')
                    path = loc.get('file', '') if loc is not None else ''
                    try:
                        path = str(Path(path).relative_to(snap))
                    except ValueError:
                        pass
                    option, message = item.get('id', ''), item.get('msg', '')
                    findings.append({'id': diagnostic_id('cppcheck', target, path, option, '',
                                                         message, hashes.get(path, 'external')),
                                     'rule': f'cppcheck:{option}', 'category': 'candidate', 'path': path,
                                     'line': int(loc.get('line', '0')) if loc is not None else 0,
                                     'symbol': '', 'message': f'{target}: {message}', 'target': target})
                status = 'complete' if proc['returncode'] in (0, 1) else 'partial'
            except ET.ParseError:
                status = 'unavailable'
            coverage.append({'tool': 'cppcheck', 'target': target, 'status': status,
                             'reason': '' if status == 'complete' else 'coverage unavailable: cppcheck failed'})
    return findings, coverage


def report(result):
    count = Counter(f['category'] for f in result['findings'])
    lines = ['# Static check run', '', f"UTC: {result['utc']}", '',
             f"HEAD: `{result['head']}`; mode: `{result['mode']}`; elapsed: {result['elapsed_seconds']:.3f}s", '',
             f"Sources: {result['metrics']['source_files']}; functions: {result['metrics']['functions']}; "
             f"new findings: {result['new_count']}; accepted: {result['accepted_count']}", '',
             'Categories: ' + ', '.join(f'{k}={v}' for k, v in sorted(count.items())), '',
             '**Lexical results are heuristics, not proofs.** Callback/assembly/macro edges and preprocessor '
             'path feasibility are not resolved. A missing guard or long function is style, not a proven defect.', '',
             '## Coverage', '']
    for item in result['coverage']:
        lines.append(f"- {item['tool']} {item.get('target', '')}: {item['status']} {item.get('reason', '')}")
    comparison = result.get('comparison')
    if comparison:
        lines += ['', '## Comparison (not review acceptance)', '',
                  f"Gate mode: {result['gate']}; newly unreviewed: {len(comparison['newly_unreviewed_ids'])}; "
                  f"existing unreviewed: {len(comparison['existing_unreviewed_ids'])}; "
                  f"reopened: {len(comparison['reopened_ids'])}; resolved: {len(comparison['resolved_ids'])}", '',
                  'Existing unreviewed findings remain unreviewed. Delta success does NOT mean a clean project.', '']
    lines += ['', '## Snapshot consistency', '',
              'Changed during scan: ' + (', '.join(result['changed_inputs']) or 'none'), '',
              'Checker changes: ' + (', '.join(result['changed_tools']) or 'none'), '',
              'See snapshot-manifest.json for every input hash and findings.json for full diagnostics.', '',
              '## Findings', '', '| Class | Rule | Location | Finding | State |',
              '|---|---|---|---|---|']
    for f in result['findings']:
        if result.get('changes_only') and f['id'] not in comparison['newly_unreviewed_ids']:
            continue
        text = f['message'].replace('|', '\\|').replace('\n', ' ')
        state = 'accepted' if f['accepted'] else 'unreviewed'
        if comparison and not f['accepted']:
            state = 'existing-unreviewed' if f['id'] in comparison['existing_unreviewed_ids'] else 'newly-unreviewed'
        lines.append(f"| {f['category']} | {f['rule']} | {f['path']}:{f['line']} {f['symbol']} | {text} | {state} |")
    lines += ['', 'No automatic baseline acceptance. Full findings.json always includes the unreviewed backlog.', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('fast', 'analyze'), default='fast')
    parser.add_argument('--targets', nargs='+', choices=('i386', 'riscv64', 'aarch64'),
                        default=['i386', 'riscv64', 'aarch64'])
    parser.add_argument('--jobs', type=int, choices=range(1, 5), default=2)
    parser.add_argument('--timeout', type=int, default=30, help='per translation unit seconds')
    parser.add_argument('--budget', type=int, default=240, help='GCC analyzer total scheduling budget seconds')
    parser.add_argument('--out', type=Path, help='report directory, defaults to a fresh /tmp directory')
    parser.add_argument('--baseline', type=Path, default=ROOT / 'tests/static/baseline.txt')
    comparison_source = parser.add_mutually_exclusive_group()
    comparison_source.add_argument('--compare', type=Path, help='explicit previous fast findings.json (not review acceptance)')
    comparison_source.add_argument('--compare-ref', help='immutable Git report source: REV:repository/path.json')
    parser.add_argument('--gate', choices=('all', 'delta'), default='all',
                        help='all: fail any unreviewed finding; delta: fail newly unreviewed only')
    parser.add_argument('--changes-only', action='store_true',
                        help='show newly unreviewed in console/Markdown; preserve full JSON')
    options = parser.parse_args()
    if options.timeout <= 0 or options.budget <= 0:
        parser.error('timeout/budget must be positive')
    has_comparison = bool(options.compare or options.compare_ref)
    if (options.gate == 'delta' or options.changes_only) and not has_comparison:
        parser.error('--gate delta/--changes-only requires an explicit comparison reference')
    if has_comparison and options.mode != 'fast':
        parser.error('comparison supports fast mode only; analyzer coverage must be inspected separately')
    start = time.monotonic()
    out = (options.out or Path(tempfile.mkdtemp(prefix='welyos-static-'))).resolve()
    # 不能把掃描結果放進共享 build 或 source tree,避免覆寫輸入與 Claude 建置。
    if out == ROOT or ROOT in out.parents:
        parser.error('--out must be outside the project (use /tmp)')
    if out.exists() and any(out.iterdir()):
        parser.error('--out must be a new or empty directory')
    out.mkdir(parents=True, exist_ok=True)
    print(f'Artifacts: {out}', flush=True)
    try:
        tool_paths = [ROOT / 'scripts/static-check.sh', options.baseline.resolve()]
        tool_paths += sorted((ROOT / 'scripts/static').glob('*.py'))
        tools = {str(p): digest(p.read_bytes()) for p in tool_paths if p.is_file()}
        # 保存工具本身,讓報告不只記住被掃描的來源版本。
        for index, path in enumerate(tool_paths):
            if path.is_file():
                dest = out / 'checker' / f'{index}-{path.name}'
                dest.parent.mkdir(exist_ok=True)
                dest.write_bytes(path.read_bytes())
        baseline = load_baseline(options.baseline)
        fingerprint = digest(Path(__file__).with_name('rules.py').read_bytes())
        reference, provenance = (load_comparison(options.compare, options.compare_ref, fingerprint)
                                 if has_comparison else (None, None))
        hashes, sources = snapshot(ROOT, out / 'source')
        head = run(['git', 'rev-parse', 'HEAD'], cwd=ROOT)['stdout'].strip()
        dirty = run(['git', 'status', '--short'], cwd=ROOT)['stdout']
        findings, metrics = check(sources)
        coverage = [{'tool': 'lexical', 'status': 'heuristic',
                     'reason': 'not a proof; loops scoped to kernel/net; source headers included'}]
        if options.mode == 'analyze':
            extra, status = analyze(out / 'source', out, hashes, options)
            findings.extend(extra)
            coverage.extend(status)
        else:
            coverage.append({'tool': 'gcc/cppcheck', 'status': 'not-requested',
                             'reason': 'fast mode; use --mode analyze for compiler-based coverage'})
        findings = sorted({f['id']: f for f in findings}.values(),
                          key=lambda f: (f['category'], f['path'], f['line'], f['id']))
        for f in findings:
            f['accepted'] = f['id'] in baseline
        comparison = compare_findings(findings, reference, provenance) if reference is not None else None
        changed = changed_inputs(hashes)
        changed_tools = [p for p, sha in tools.items() if not Path(p).is_file() or
                         digest(Path(p).read_bytes()) != sha]
        new_count = sum(not f['accepted'] for f in findings)
        gate_count = len(comparison['newly_unreviewed_ids']) if options.gate == 'delta' else new_count
        incomplete = bool(changed or changed_tools) or any(c['status'] in ('partial', 'unavailable') for c in coverage)
        result = {'schema_version': SCHEMA_VERSION, 'rules_fingerprint': fingerprint, 'scan_scope': SCAN_SCOPE,
                  'utc': datetime.now(timezone.utc).isoformat(), 'head': head, 'git_status': dirty,
                  'mode': options.mode, 'metrics': metrics, 'coverage': coverage,
                  'gate': options.gate, 'gate_count': gate_count, 'comparison': comparison,
                  'changes_only': options.changes_only,
                  'changed_inputs': changed, 'changed_tools': changed_tools, 'findings': findings, 'new_count': new_count,
                  'accepted_count': len(findings) - new_count,
                  'baseline_stale_ids': sorted(set(baseline) - {f['id'] for f in findings}),
                  'elapsed_seconds': time.monotonic() - start,
                  'exit_code': 2 if incomplete else (1 if gate_count else 0)}
        (out / 'snapshot-manifest.json').write_text(json.dumps(hashes, indent=2) + '\n')
        (out / 'tools-manifest.json').write_text(json.dumps(tools, indent=2) + '\n')
        (out / 'findings.json').write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n')
        (out / 'REPORT.md').write_text(report(result))
        print(f"{options.mode}: {metrics['source_files']} files, {metrics['functions']} functions, "
              f"{new_count} NEW findings, {result['elapsed_seconds']:.3f}s", flush=True)
        if comparison:
            print(f"comparison: {len(comparison['newly_unreviewed_ids'])} newly unreviewed; "
                  f"{len(comparison['existing_unreviewed_ids'])} existing-unreviewed; "
                  f"{len(comparison['reopened_ids'])} reopened; {len(comparison['resolved_ids'])} resolved")
            if options.changes_only:
                for f in findings:
                    if f['id'] in comparison['newly_unreviewed_ids']:
                        print(f"{f['path']}:{f['line']} [{f['rule']}] {f['message']}")
            if options.gate == 'delta':
                print('DELTA gate only: previously seen unreviewed findings are NOT accepted; this is NOT a full clean result.')
        for c in coverage:
            print(f"coverage: {c['tool']} {c.get('target', '')} {c['status']} {c.get('reason', '')}")
        if changed:
            print('STALE: inputs changed during scan: ' + ', '.join(changed))
        print(f"exit={result['exit_code']} (0=selected gate passed; 1=unreviewed findings in selected gate; "
              "2=incomplete/stale/tool error)")
        return result['exit_code']
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        print(f'coverage unavailable: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())

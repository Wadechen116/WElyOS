"""驗證掃描器會找到刻意植入的問題,且 gate/基準不會悄悄吞掉新問題。"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/static'))
sys.dont_write_bytecode = True
import rules
import check as runner


class RulesTest(unittest.TestCase):
    def scan(self, text, path='kernel/net/example.c'):
        return rules.check({path: text})[0]

    def test_comments_literals_and_guard(self):
        found = self.scan('''
            // strcpy(x,y); void fake(void) { fake(); }
            void f(int n) { const char *s="strcpy(x,y)"; if (n) use(s); }
        ''')
        self.assertFalse(found)

    def test_error_literal_does_not_create_error_contract(self):
        found = self.scan('''
            void text(int n) { if(n)use("return -E_IO"); }
            void caller(int n) { if(n)text(n); }
        ''')
        self.assertFalse(any(f['rule']=='unchecked-error' for f in found))

    def test_hash_includes_literals_not_line_offsets(self):
        a = self.scan('void f(void) { use("a"); }')[0]
        b = self.scan('\n\nvoid f(void) { use("a"); }')[0]
        c = self.scan('void f(void) { use("b"); }')[0]
        self.assertEqual(a['id'], b['id'])
        self.assertNotEqual(a['id'], c['id'])

    def test_unsafe_and_unchecked(self):
        found = self.scan('''
            int err(int p) { if(p) return -E_IO; return 0; }
            void caller(int n) {
                if(n) { strcpy(dst,src); err(n); (void)err(n); }
                int r=err(n); if(err(n)) use(r);
            }
        ''')
        self.assertEqual(sum(f['rule']=='banned-call' for f in found), 1)
        self.assertEqual(sum(f['rule']=='unchecked-error' for f in found), 2)

    def test_error_result_propagates_through_local(self):
        found = self.scan('''
            int err(void) { return -E_IO; }
            int wrap(void) { int rc=err(); return rc; }
            void caller(void) { wrap(); }
        ''')
        self.assertTrue(any(f['rule']=='unchecked-error' and f['symbol']=='caller' for f in found))

    def test_cycle_and_user_core_separation(self):
        found, _ = rules.check({
            'kernel/a.c': 'void a(void){b();} void b(void){a();}',
            'user/a.c': 'void a(void){user_only();} void user_only(void){}',
        })
        cycles = [f for f in found if f['rule']=='recursion']
        self.assertEqual(len(cycles), 3)
        self.assertFalse(any('user/' in f['message'] for f in cycles))

    def test_architecture_graphs_do_not_mix(self):
        found, _ = rules.check({
            'arch/i386/a.c': 'void a(void){b();}',
            'arch/riscv64/a.c': 'void b(void){a();}',
        })
        self.assertFalse(any(f['rule']=='recursion' for f in found))

    def test_loop_bound_and_scope(self):
        source = 'void f(int n){for(int i=0;i<n;++i)use(i); while(1)use(n);}'
        self.assertEqual(sum(f['rule']=='loop-bound' for f in self.scan(source)), 1)
        self.assertFalse(any(f['rule']=='loop-bound' for f in self.scan(source, 'kernel/other.c')))

    def test_no_allocation_reachable_after_init(self):
        source = '''/* no-alloc-after-init */
            void setup_init(void){kmalloc(4);}
            void helper(void){pmm_alloc(1);}
            void runtime(void){helper();}
        '''
        found = self.scan(source)
        names = {f['symbol'] for f in found if f['rule']=='allocation-after-init'}
        self.assertEqual(names, {'helper', 'runtime'})

    def test_length(self):
        found = self.scan('void big(int n) {\n' + 'use(n);\n'*81 + '}')
        self.assertTrue(any(f['rule']=='function-length' for f in found))


class GateTest(unittest.TestCase):
    def test_baseline_schema(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'baseline.txt'
            for value in ('[]', '{"id":12}', '{"id":"'+'0'*24+'"}'):
                path.write_text(value)
                with self.assertRaises(ValueError):
                    runner.load_baseline(path)

    def test_gate_clean_new_reviewed_changed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'source'
            root.mkdir()
            (root/'CMakeLists.txt').write_text('')
            for tree in runner.TREES:
                (root/tree).mkdir()
            path = root/'kernel/a.c'
            baseline = root/'baseline.txt'
            baseline.write_text('')
            def gate(label):
                out = Path(directory) / label
                with mock.patch.object(runner, 'ROOT', root), mock.patch.object(
                        sys, 'argv', ['check.py', '--out', str(out), '--baseline', str(baseline)]), \
                        contextlib.redirect_stdout(io.StringIO()):
                    rc = runner.main()
                return rc, json.loads((out/'findings.json').read_text())
            path.write_text('void f(int n){if(n)return;}')
            self.assertEqual(gate('clean')[0], 0)
            path.write_text('void f(int n){if(n)strcpy(dst,"a");}')
            rc, result = gate('new')
            self.assertEqual(rc, 1)
            self.assertEqual(result['new_count'], 1)
            item = {'id': result['findings'][0]['id'], 'reviewer':'fixture reviewer',
                    'date':'2026-11-18', 'reason':'fixture-specific exception'}
            baseline.write_text(json.dumps(item)+'\n')
            self.assertEqual(gate('reviewed')[0], 0)
            path.write_text('void f(int n){if(n)strcpy(dst,"different");}')
            rc, result = gate('changed')
            self.assertEqual(rc, 1)
            self.assertEqual(result['baseline_stale_ids'], [item['id']])

    def test_change_detection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'CMakeLists.txt').write_text('one')
            with mock.patch.object(runner, 'ROOT', root):
                hashes = {'CMakeLists.txt': runner.digest(b'one')}
                self.assertEqual(runner.changed_inputs(hashes), [])
                (root/'CMakeLists.txt').write_text('two')
                self.assertEqual(runner.changed_inputs(hashes), ['CMakeLists.txt'])

    def test_gcc_complexity_and_invalid_diagnostics(self):
        text = json.dumps([{'kind':'warning', 'option':'-Wanalyzer-too-complex',
                           'message':'giving up', 'locations':[{'caret':{'file':'/snap/kernel/a.c','line':3}}]}])
        found, exhausted = runner.gcc_diagnostics(text, Path('/snap'), 'i386', {'kernel/a.c':'hash'})
        self.assertTrue(exhausted)
        self.assertEqual(found[0]['category'], 'coverage')
        with self.assertRaises(ValueError):
            runner.gcc_diagnostics('{}', Path('/snap'), 'i386', {})

    def test_missing_cmake_is_not_pass(self):
        class Options:
            targets = ['i386']
        with mock.patch.object(runner.shutil, 'which', return_value=None):
            found, coverage = runner.analyze(Path('/snap'), Path('/out'), {}, Options())
        self.assertEqual(found, [])
        self.assertTrue(any(c['status']=='unavailable' for c in coverage))

    def test_timeout_is_not_success(self):
        result = runner.run([sys.executable, '-c', 'import time; time.sleep(2)'], timeout=0.02)
        self.assertIsNone(result['returncode'])
        self.assertIn('timeout', result['stderr'])


if __name__ == '__main__':
    unittest.main()

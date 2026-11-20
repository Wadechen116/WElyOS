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

    def test_unchecked_bodies_do_not_inherit_condition_assignments(self):
        for control in ('while ((n = err()) > 0)', 'if ((n = err()) > 0)',
                        'for (int i = 0; i < n; i++)',
                        'while ((n = err()) > 0) if ((k = err()) > 0)'):
            with self.subTest(control=control):
                found = self.scan('int err(void) { return -E_IO; } '
                                  'void caller(int n,int k) { '+control+' err(); }')
                unchecked = [f for f in found if f['rule']=='unchecked-error']
                self.assertEqual(len(unchecked),1)
                self.assertEqual(unchecked[0]['symbol'],'caller')

    def test_control_body_checked_results_still_count_as_used(self):
        for body in ('{ int r=err(); use(r); }', 'n=err();', 'return err();',
                     'if (err()) use(n);', 'CHECK(err());'):
            with self.subTest(body=body):
                found = self.scan('int err(void) { return -E_IO; } '
                                  'int caller(int n) { while ((n=err()) > 0) '+body+' }')
                self.assertFalse(any(f['rule']=='unchecked-error' for f in found))

    def test_control_body_void_discard_and_else(self):
        found = self.scan('int err(void) { return -E_IO; } '
                          'void caller(int n) { '
                          'if ((n=err())) (void)err(); else err(); '
                          'do err(); while ((n=err()) > 0); }')
        self.assertEqual(sum(f['rule']=='unchecked-error' for f in found),3)

    def test_error_result_propagates_through_local(self):
        found = self.scan('''
            int err(void) { return -E_IO; }
            int wrap(void) { int rc=err(); return rc; }
            void caller(void) { wrap(); }
        ''')
        self.assertTrue(any(f['rule']=='unchecked-error' and f['symbol']=='caller' for f in found))

    def test_pte_direct_store_guard(self):
        for statement in ('*entry = v;', 'entry[i] = v;', '(*entry) = v;',
                          '*entry |= v;', 'entry[i]++;', '++*entry;',
                          'vs->root[i] = v;'):
            with self.subTest(statement=statement):
                found = self.scan('pte_t *root; void f(pte_t *entry, int i, int v) { '
                                  + statement + ' }', path='kernel/vm.c')
                self.assertTrue(any(f['rule']=='pte-direct-store' for f in found))
        good = self.scan('pte_t *root; void f(pte_t *entry, int v) { '
                         'pte_t *next = entry; next = root; '
                         'arch_pte_set(entry,v); read(entry[0]); }', path='kernel/vm.c')
        self.assertFalse(any(f['rule']=='pte-direct-store' for f in good))

    def test_pte_store_guard_alias_comments_scope(self):
        source = 'void f(pte_t *entry) { pte_t *alias=entry; alias[0]=1; }'
        found = self.scan(source, path='kernel/vm.c')
        self.assertTrue(any(f['rule']=='pte-direct-store' for f in found))
        other = self.scan(source, path='user/example.c')
        self.assertFalse(any(f['rule']=='pte-direct-store' for f in other))
        comments = self.scan('void f(pte_t *entry) { '
                             '// *entry=1;\n use("entry[0]=1;"); }', path='kernel/vm.c')
        self.assertFalse(any(f['rule']=='pte-direct-store' for f in comments))

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


class ComparisonTest(unittest.TestCase):
    @staticmethod
    def item(number, accepted=False):
        return {'id': f'{number:024x}', 'accepted': accepted, 'path':'kernel/a.c',
                'rule':'banned-call', 'category':'candidate'}

    def reference(self, findings):
        return {'schema_version': runner.SCHEMA_VERSION, 'mode':'fast',
                'scan_scope': runner.SCAN_SCOPE, 'rules_fingerprint':'fixture-rules',
                'changed_inputs':[], 'changed_tools':[],
                'coverage':[{'tool':'lexical','status':'heuristic'},
                            {'tool':'gcc/cppcheck','status':'not-requested'}],
                'exit_code': 1 if any(not f['accepted'] for f in findings) else 0,
                'new_count':sum(not f['accepted'] for f in findings),
                'accepted_count':sum(f['accepted'] for f in findings), 'findings':findings}

    def test_existing_reopened_added_resolved_remain_distinct(self):
        old = self.reference([self.item(1),self.item(2,True),self.item(3)])
        current = [self.item(1),self.item(2),self.item(4),self.item(5,True)]
        delta = runner.compare_findings(current,old,{'kind':'fixture'})
        self.assertEqual(delta['existing_unreviewed_ids'],[self.item(1)['id']])
        self.assertEqual(delta['newly_unreviewed_ids'],[self.item(2)['id'],self.item(4)['id']])
        self.assertEqual(delta['reopened_ids'],[self.item(2)['id']])
        self.assertEqual(delta['resolved_ids'],[self.item(3)['id']])
        self.assertEqual(delta['introduced_ids'],[self.item(4)['id'],self.item(5)['id']])
        self.assertFalse(current[0]['accepted'])

    def test_reference_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'report.json'
            good = self.reference([self.item(1)])
            path.write_text(json.dumps(good))
            loaded, provenance = runner.load_comparison(path,None,'fixture-rules')
            self.assertEqual(loaded,good)
            self.assertEqual(provenance['kind'],'file')
            cases = [dict(good,mode='analyze'),dict(good,schema_version=0),
                     dict(good,rules_fingerprint='changed'),dict(good,scan_scope={}),
                     dict(good,changed_inputs=['kernel/a.c']),dict(good,changed_tools=['rules.py']),
                     dict(good,exit_code=2),dict(good,findings=[self.item(1),self.item(1)]),
                     dict(good,new_count=0),dict(good,coverage=[{'tool':'gcc','status':'unavailable'}]),
                     dict(good,findings=[dict(self.item(1),accepted='false')])]
            for bad in cases:
                with self.subTest(bad=bad):
                    path.write_text(json.dumps(bad))
                    with self.assertRaises(ValueError):
                        runner.load_comparison(path,None,'fixture-rules')

    def test_git_reference_is_pinned_without_mutating_git(self):
        report = self.reference([self.item(1)])
        commit = 'a'*40
        replies = [{'returncode':0,'stdout':commit+'\n','stderr':''},
                   {'returncode':0,'stdout':json.dumps(report),'stderr':''}]
        with mock.patch.object(runner,'run',side_effect=replies) as invoke:
            loaded, provenance = runner.load_comparison(None,'HEAD:tests/static/ref.json','fixture-rules')
        self.assertEqual(loaded,report)
        self.assertEqual(provenance['commit'],commit)
        self.assertEqual(invoke.call_args_list[1].args[0],['git','show',commit+':tests/static/ref.json'])
        for bad in ('HEAD',':ref.json','-x:ref.json','HEAD:/outside.json','HEAD:../outside.json'):
            with self.assertRaises(ValueError):
                runner.load_comparison(None,bad,'fixture-rules')

    def test_cli_delta_is_opt_in_and_preserves_unreviewed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'project'
            root.mkdir()
            (root/'CMakeLists.txt').write_text('')
            for tree in runner.TREES:
                (root/tree).mkdir()
            source = root/'kernel/a.c'
            source.write_text('void f(int n){if(n)strcpy(dst,"old");}')
            baseline = root/'baseline.txt'
            baseline.write_text('')
            def gate(label, *options):
                out = Path(directory)/label
                with mock.patch.object(runner,'ROOT',root), mock.patch.object(
                        sys,'argv',['check.py','--out',str(out),'--baseline',str(baseline),*options]), \
                        contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    rc = runner.main()
                path = out/'findings.json'
                return rc, json.loads(path.read_text()) if path.exists() else None, path
            rc, first, ref = gate('first')
            self.assertEqual(rc,1)
            rc, result, _ = gate('same','--compare',str(ref),'--gate','delta','--changes-only')
            self.assertEqual(rc,0)
            self.assertEqual(result['new_count'],1)
            self.assertEqual(result['accepted_count'],0)
            self.assertEqual(len(result['comparison']['existing_unreviewed_ids']),1)
            self.assertEqual(baseline.read_text(),'')
            self.assertEqual(gate('strict','--compare',str(ref))[0],1)
            source.write_text('void f(int n){if(n)strcpy(dst,"new");}')
            rc, result, _ = gate('new','--compare',str(ref),'--gate','delta')
            self.assertEqual(rc,1)
            self.assertEqual(len(result['comparison']['newly_unreviewed_ids']),1)
            key = result['findings'][0]['id']
            baseline.write_text(json.dumps({'id':key,'reviewer':'fixture','reason':'fixture','date':'2026-11-18'}))
            rc, accepted, reviewed_ref = gate('accepted')
            self.assertEqual(rc,0)
            baseline.write_text('')
            rc, result, _ = gate('reopened','--compare',str(reviewed_ref),'--gate','delta')
            self.assertEqual(rc,1)
            self.assertEqual(result['comparison']['reopened_ids'],[key])
            invalid = Path(directory)/'invalid.json'
            invalid.write_text('{}')
            self.assertEqual(gate('invalid','--compare',str(invalid),'--gate','delta')[0],2)

    def test_parser_rejects_implicit_or_analyzer_delta(self):
        for argv in (['--gate','delta'],['--changes-only'],
                     ['--mode','analyze','--compare','unused.json']):
            with mock.patch.object(sys,'argv',['check.py',*argv]), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as exc:
                    runner.main()
            self.assertEqual(exc.exception.code,2)


if __name__ == '__main__':
    unittest.main()

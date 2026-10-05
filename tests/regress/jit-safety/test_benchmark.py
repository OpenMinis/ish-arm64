"""Host-only accounting tests; no guest, network or workload installation."""
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('runner', ROOT / 'benchmark/aojit/guest/run_cases.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

class Accounting(unittest.TestCase):
    def run_case(self, samples, repeat=1, volatile=False, modes=('on', 'off')):
        case = dict(id='test', title='test', group='group', volatile=volatile)
        with patch.object(runner, 'set_mode'), patch.object(runner, 'module_counts', return_value={}), \
             patch.object(runner, 'log'), patch.object(runner, 'run_once', side_effect=samples):
            return runner.run_case(case, SimpleNamespace(repeat=repeat, timeout=1), list(modes), 9000)[0]

    def test_nonzero_exits_never_timed(self):
        r = self.run_case([(0.001, 1, 'error')] * 2)
        self.assertEqual(r['times'], {'on': [], 'off': []})
        self.assertNotIn('speedup', r)
        self.assertEqual(r['status'], 'failed')

    def test_partial_failures_do_not_cherry_pick(self):
        r = self.run_case([(1, 0, 'ok'), (2, 0, 'ok'), (0.01, 1, 'bad'), (1, 0, 'ok')], repeat=2)
        self.assertEqual(r['times'], {'on': [1, 1], 'off': [2]})
        self.assertNotIn('speedup', r)
        self.assertEqual(r['status'], 'failed')

    def test_timeout(self):
        r = self.run_case([(None, 'timeout', ''), (2, 0, 'ok')])
        self.assertEqual(r['times']['on'], [])
        self.assertNotIn('speedup', r)
        self.assertEqual(r['status'], 'failed')

    def test_mismatched_output(self):
        r = self.run_case([(1, 0, 'a'), (2, 0, 'b')])
        self.assertFalse(r['same_output'])
        self.assertNotIn('speedup', r)
        self.assertEqual(r['status'], 'failed')

    def test_within_mode_nondeterminism(self):
        r = self.run_case([(1, 0, 'a'), (2, 0, 'a'), (2, 0, 'a'), (1, 0, 'b')], repeat=2)
        self.assertFalse(r['same_output'])
        self.assertNotIn('speedup', r)

    def test_valid_speedup(self):
        r = self.run_case([(1, 0, 'a'), (2, 0, 'a'), (4, 0, 'a'), (3, 0, 'a')], repeat=2)
        self.assertEqual(r['speedup'], 2)
        self.assertEqual(r['status'], 'passed')

    def test_volatile_is_unverified_not_equal(self):
        r = self.run_case([(1, 0, 'a'), (2, 0, 'b')], volatile=True)
        self.assertIsNone(r['same_output'])
        self.assertEqual(r['status'], 'unverified')
        self.assertNotIn('speedup', r)
        self.assertIn('unverified', runner.summarize([r], ['on','off']))

    def test_volatile_failures_still_fail(self):
        r = self.run_case([(0.001, 1, 'error')] * 2, volatile=True)
        self.assertEqual(r['status'], 'failed')
        self.assertNotIn('speedup', r)

    def test_current_mode_is_not_comparison(self):
        r = self.run_case([(1, 0, 'a')], modes=('cur',))
        self.assertEqual(r['status'], 'timing-only')
        self.assertNotIn('speedup', r)

    def test_submillisecond_precision(self):
        r = self.run_case([(0.0001, 0, 'a'), (0.0002, 0, 'a')])
        self.assertEqual(r['speedup'], 2)
        self.assertGreater(r['best']['on'], 0)

    def test_invalid_durations(self):
        for value in [None, 0, -1, float('nan'), float('inf')]:
            with self.subTest(value=value):
                r = self.run_case([(value, 0, 'a'), (2, 0, 'a')])
                self.assertEqual(r['status'], 'failed')
                self.assertNotIn('speedup', r)

    def cli(self, args=(), samples=None, skip=False, setup=False, volatile=False, no_jit=False):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)
            (p/'cases.json').write_text(json.dumps({'cases':[dict(id='test',title='test',group='g',tier='quick',volatile=volatile)]}))
            if not no_jit: (p/'jit').touch()
            if not setup: (p/'.setup_done').touch()
            with patch.object(runner,'A',d), patch.object(runner,'JIT',str(p/'jit')), \
                 patch.object(runner,'tqdm',None), patch.object(runner,'log'), \
                 patch.object(runner,'env_info',return_value=dict(version='test',date='',packages=[])), \
                 patch.object(runner,'run_once',side_effect=samples), \
                 patch.object(runner,'missing_needs',return_value='absent' if skip else None), \
                 patch.object(runner.subprocess,'run',return_value=SimpleNamespace(returncode=1)), \
                 patch.object(runner.sys,'argv',['runner','-r','1','--out',str(p/'result'),*args]):
                rc=runner.main()
                data=json.loads((p/'result.json').read_text()) if (p/'result.json').exists() else None
                return rc,data

    def test_cli_failure(self):
        rc,data=self.cli(samples=[(1,1,'error')]*2)
        self.assertEqual(rc,1)
        self.assertEqual(data['cases'][0]['status'],'failed')
        self.assertNotIn('speedup',data['cases'][0])

    def test_cli_mismatch(self):
        rc,_=self.cli(samples=[(1,0,'a'),(2,0,'b')])
        self.assertEqual(rc,1)

    def test_cli_success(self):
        rc,data=self.cli(samples=[(1,0,'a'),(2,0,'a')])
        self.assertEqual(rc,0)
        self.assertEqual(data['cases'][0]['speedup'],2)

    def test_cli_volatile(self):
        rc,data=self.cli(samples=[(1,0,'a'),(2,0,'b')], volatile=True)
        self.assertEqual(rc,2)
        self.assertEqual(data['cases'][0]['status'],'unverified')
        self.assertNotIn('speedup',data['cases'][0])

    def test_cli_missing_jit_requires_explicit_cur(self):
        rc,data=self.cli(no_jit=True)
        self.assertEqual(rc,2)
        self.assertIsNone(data)
        rc,data=self.cli(no_jit=True,args=('--modes','cur'),samples=[(1,0,'a')])
        self.assertEqual(rc,0)
        self.assertEqual(data['cases'][0]['status'],'timing-only')
        self.assertNotIn('speedup',data['cases'][0])

    def test_cli_all_skipped(self):
        rc,data=self.cli(skip=True)
        self.assertEqual(rc,2)
        self.assertIn('skipped',data['cases'][0])

    def test_cli_setup_failure(self):
        rc,data=self.cli(setup=True)
        self.assertEqual(rc,1)
        self.assertIsNone(data)

    def test_cli_bad_arguments(self):
        for args in [('-r','0'),('--timeout','0'),('--modes','on,evil'),('--modes','on,on'),('--modes',''),('--modes','cur,on')]:
            with self.subTest(args=args), self.assertRaises(SystemExit) as e:
                self.cli(args=args)
            self.assertEqual(e.exception.code,2)

if __name__ == '__main__': unittest.main()

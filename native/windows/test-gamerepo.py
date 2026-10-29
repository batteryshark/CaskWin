"""Validate registry-view staging without launching games or changing a real repo."""
import contextlib
import io
import json
import pathlib
import tempfile
import unittest
from unittest import mock

import gamerepo


class RegistryViewStaging(unittest.TestCase):
    def test_explicit_and_default_views(self):
        self.assertEqual(gamerepo.registry_view({}), '64')
        self.assertEqual(gamerepo.registry_view({'environment': {'architecture': 'win64'}}), '64')
        self.assertEqual(gamerepo.registry_view({'environment': {'architecture': 'win32'}}), '32')
        with self.assertRaises(ValueError):
            gamerepo.registry_view({'environment': {'architecture': 'unsupported'}})

    def test_registry_architecture_does_not_select_loader_or_program_bitness(self):
        workspace = pathlib.Path(__file__).parent/'test-output'
        workspace.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='gamerepo-registry-', dir=workspace) as folder:
            root = pathlib.Path(folder)
            archive = root/'repo'/'archive'/'game'/'1'
            archive.mkdir(parents=True)
            manifest = {'format': 'cryo/3.0', 'environment': {'architecture': 'win32'},
                        'registry': ['base.reg'], 'entrypoints': [{'exec': r'C:\app\actual64.exe'}]}
            (archive/'manifest.yaml').write_text(gamerepo.yaml.safe_dump(manifest), encoding='utf-8')
            (archive/'base.reg').write_text('REGEDIT4\n', encoding='utf-8')
            prefix = root/'prefix'
            wine = root/'runtime'/'wine64.exe'

            def fake_run(command, **kwargs):
                self.assertTrue(kwargs['check'])
                if command[-1] == 'init':
                    (prefix/'.winedows').mkdir(parents=True)

            args = ['gamerepo.py', '--repo', str(root/'repo'), '--game', 'game', '--version', '1',
                    '--prefix', str(prefix), '--wine', str(wine)]
            with mock.patch('sys.argv', args), mock.patch.object(gamerepo.subprocess, 'run', side_effect=fake_run) as run, contextlib.redirect_stdout(io.StringIO()):
                gamerepo.main()
            self.assertEqual(len(run.call_args_list), 2)
            command = run.call_args_list[1].args[0]
            self.assertEqual(command[0], str(wine.resolve()))
            self.assertEqual(command[-4:], ['reg', 'import', str((archive/'base.reg').resolve()), '/reg:32'])
            report = json.loads((prefix/'.winedows'/'gamerepo.json').read_text(encoding='utf-8'))
            self.assertEqual(report['registry_view'], '32')
            self.assertEqual(report['entrypoints'], manifest['entrypoints'])
            self.assertEqual((archive/'base.reg').read_text(encoding='utf-8'), 'REGEDIT4\n')


if __name__ == '__main__':
    unittest.main()

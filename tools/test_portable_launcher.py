#!/usr/bin/env python3
"""Check portable settings setup without starting a GUI or reading real settings."""
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FAKE = '''#!/usr/bin/env python3
import json, os, sys
print(json.dumps({'config': os.environ.get('XDG_CONFIG_HOME'), 'args': sys.argv[1:], 'cwd': os.getcwd()}))
'''


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def executable(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(FAKE)
    # Intentionally leave it non-executable: the launcher handles downloaded files.
    path.chmod(0o644)
    return path


def launch(script, cwd, env, *args):
    result = subprocess.run(['bash', str(script), *map(str, args)], cwd=cwd,
                            env=env, capture_output=True, text=True)
    require(result.returncode == 0, result.stderr)
    return json.loads(result.stdout)


def main():
    with tempfile.TemporaryDirectory(prefix='melonds-launcher-test-') as directory:
        root = Path(directory)
        bundle = root / 'bundle with spaces'
        bundle.mkdir()
        script = bundle / 'run-melonds.sh'
        shutil.copyfile(ROOT / 'run-melonds.sh', script)
        cwd = root / 'different working directory'
        cwd.mkdir()
        system = root / 'system config'
        system.mkdir()
        env = dict(os.environ, XDG_CONFIG_HOME=str(system))
        env.pop('MELONDS_BINARY', None)
        (system / 'melonDS').mkdir()
        source = system / 'melonDS/melonDS.toml'
        source.write_text('original = true\n')
        image = executable(bundle / f'melonDS-{platform.machine()}.AppImage')
        settings = Path(str(image) + '.config') / 'melonDS'
        args = ['rom with spaces.nds', 'literal $(echo unchanged)']
        result = launch(script, cwd, env, '--', *args)
        require(result['args'] == args and result['cwd'] == str(cwd), 'arguments and working directory')
        require(result['config'] == str(settings.parent), 'AppImage portable XDG directory')
        require((settings / 'melonDS.toml').read_text() == source.read_text(), 'first-run import')
        require(os.access(image, os.X_OK), 'make downloaded executable runnable')

        (settings / 'melonDS.toml').write_text('portable = true\n')
        source.write_text('system_changed = true\n')
        launch(script, cwd, env)
        launch(script, cwd, env, '--fresh')
        require((settings / 'melonDS.toml').read_text() == 'portable = true\n', 'existing settings preserved')
        require(source.read_text() == 'system_changed = true\n', 'system settings untouched')

        fresh = executable(root / 'fresh image.AppImage')
        launch(script, cwd, env, '--binary', fresh, '--fresh')
        fresh_dir = Path(str(fresh) + '.config') / 'melonDS'
        require(fresh_dir.is_dir() and not (fresh_dir / 'melonDS.toml').exists(), 'fresh mode skips import')

        native = executable(root / 'native build/melonDS')
        result = launch(script, cwd, env, '--binary', native, '--', '--help')
        require((native.parent / 'portable/melonDS.toml').read_text() == source.read_text(), 'native portable folder')
        require(result['config'] == str(system) and result['args'] == ['--help'], 'native environment and forwarding')

        link = root / 'image link.AppImage'
        link.symlink_to(image)
        result = launch(script, cwd, env, '--binary', link)
        require(result['config'] == str(settings.parent), 'symlink resolves to actual image settings')

        explicit = executable(root / 'env image.AppImage')
        result = launch(script, cwd, dict(env, MELONDS_BINARY=str(explicit)))
        require(result['config'] == str(explicit) + '.config', 'executable environment override')

        absent = subprocess.run(['bash', str(script), '--binary', str(root / 'missing.AppImage')],
                                cwd=cwd, env=env, capture_output=True, text=True)
        require(absent.returncode != 0 and 'executable not found' in absent.stderr, 'missing executable error')
    print('PASS: portable setup, import, no overwrite, fresh mode, native/AppImage, symlinks, spaces, arguments and missing files')


if __name__ == '__main__':
    main()

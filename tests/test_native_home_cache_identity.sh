#!/usr/bin/env bash
# Native POSIX startup, workers, and supervisors must use the literal HOME cache.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="${CBM_TEST_BINARY:-${ROOT}/build/c/codebase-memory-mcp}"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "ok: Windows separators covered by native platform/workspace suites"
        exit 0
        ;;
esac
source "${ROOT}/scripts/test-runtime.sh"
cbm_test_runtime_init
trap 'cbm_test_runtime_cleanup "$BINARY"' EXIT
python3 - "$BINARY" "$CBM_TEST_RUNTIME_ROOT" <<'PY'
import hashlib
import json
import os
import pathlib
import subprocess
import sys

binary = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(sys.argv[2])
fingerprint = hashlib.sha256(binary.read_bytes()).hexdigest()
for mode in ('startup', 'worker', 'activation', 'supervisor'):
    fixture = root / mode
    fixture.mkdir(mode=0o700)
    home = fixture / 'home\\literal'
    home.mkdir(mode=0o700)
    env = os.environ.copy()
    env['HOME'] = str(home)
    env['SHELL'] = '/bin/zsh'
    env.pop('USERPROFILE', None)
    env.pop('CBM_CACHE_DIR', None)
    if mode == 'startup':
        command = [str(binary), 'daemon', 'status']
    elif mode == 'worker':
        response = fixture / 'worker.response'
        args = json.dumps({'repo_path': str(fixture / 'does-not-exist'), 'mode': 'fast'})
        command = [str(binary), 'cli', '--index-worker', '--index-worker-build',
                   fingerprint, 'index_repository', args, '--response-out', str(response)]
    elif mode == 'activation':
        command = [str(binary), 'install', '--force', '--skip-config', '--yes',
                   '--dir', str(fixture / 'bin')]
    else:
        command = [str(binary), 'cli', 'index_repository', '--repo-path',
                   str(fixture / 'does-not-exist'), '--mode', 'fast']
    try:
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
    finally:
        if mode == 'supervisor':
            subprocess.run([str(binary), 'daemon', 'stop'], env=env,
                           capture_output=True, timeout=30)
    if mode == 'startup':
        assert result.returncode == 1 and 'not running' in result.stdout, (
            result.returncode, result.stdout, result.stderr)
    elif mode == 'worker':
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        assert response.is_file() and 'Pipeline failed' in response.read_text(), (
            result.stdout, result.stderr)
    elif mode == 'activation':
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    else:
        output = result.stdout + result.stderr
        assert result.returncode != 0 and 'Pipeline failed' in output, (
            result.returncode, result.stdout, result.stderr)
        assert 'crashed on a file' not in output, output
    cache = home / '.cache/codebase-memory-mcp'
    rewritten = fixture / 'home/literal'
    assert cache.is_dir(), f'{mode}: native cache missing: {cache}'
    assert not rewritten.exists(), f'{mode}: created slash-rewritten HOME: {rewritten}'
    if mode == 'activation':
        assert (cache / 'logs/activation-events.ndjson').is_file(), 'native activation log missing'
    print(f'ok: {mode} keeps literal POSIX HOME cache; no rewritten tree')

if sys.platform.startswith('linux'):
    fixture = root / 'config-consumers'
    home = fixture / 'home'
    (home / '.config/Code/User').mkdir(parents=True, mode=0o700)
    config = str(fixture / 'long-config')
    while len(config.encode()) < 3000:
        remaining = 3000 - len(config.encode())
        component = 60 if remaining > 61 else remaining - 1
        if remaining == 62:
            component = 59
        config += '/' + 'c' * component
    code_user = pathlib.Path(config) / 'Code/User'
    profile = code_user / 'profiles/fixture-profile'
    profile.mkdir(parents=True, mode=0o700)
    env = os.environ.copy()
    env.update(HOME=str(home), XDG_CONFIG_HOME=config, SHELL='/bin/zsh', PATH='/usr/bin:/bin')
    env.pop('USERPROFILE', None)
    env.pop('CBM_CACHE_DIR', None)
    installed = home / '.local/bin/codebase-memory-mcp'
    result = subprocess.run([str(binary), 'install', '--force', '--yes', '--clients=vscode'],
                            cwd=fixture, env=env, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    configs = [code_user / 'mcp.json', profile / 'mcp.json']
    for path in configs:
        assert path.is_file() and str(installed) in path.read_text(), f'complete config missing: {path}'
    result = subprocess.run([str(binary), 'uninstall', '--yes'], cwd=fixture, env=env,
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    for path in configs:
        assert 'codebase-memory-mcp' not in path.read_text(), f'owned config survived: {path}'
    print('ok: long native config override reaches default and profile install/uninstall')
PY

#!/usr/bin/env bash
# Native POSIX startup and direct workers must use the literal HOME cache.
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
for mode in ('startup', 'worker', 'activation'):
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
    else:
        command = [str(binary), 'install', '--force', '--skip-config', '--yes',
                   '--dir', str(fixture / 'bin')]
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
    if mode == 'startup':
        assert result.returncode == 1 and 'not running' in result.stderr, result.stderr
    elif mode == 'worker':
        assert result.returncode == 0, result.stderr
        assert response.is_file() and 'Pipeline failed' in response.read_text(), result.stderr
    else:
        assert result.returncode == 0, result.stderr
    cache = home / '.cache/codebase-memory-mcp'
    rewritten = fixture / 'home/literal'
    assert cache.is_dir(), f'{mode}: native cache missing: {cache}'
    assert not rewritten.exists(), f'{mode}: created slash-rewritten HOME: {rewritten}'
    if mode == 'activation':
        assert (cache / 'logs/activation-events.ndjson').is_file(), 'native activation log missing'
    print(f'ok: {mode} keeps literal POSIX HOME cache; no rewritten tree')
PY

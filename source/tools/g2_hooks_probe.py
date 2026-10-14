#!/usr/bin/env python3
"""Installed Codex + local synthetic SSE, isolated settings; no credentials/API."""
import argparse
import http.server
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import threading
import time


def observe(root, label, transcript=None):
    paths = [Path(transcript)] if transcript else list((root / 'home' / 'sessions').rglob('*.jsonl'))
    states = []
    for p in paths:
        try: raw = p.read_bytes()
        except FileNotFoundError: continue
        kinds = []
        for row in raw.splitlines():
            try:
                j = json.loads(row); payload = j.get('payload', {})
                kinds.append([j.get('type'), payload.get('type'), payload.get('role')])
            except json.JSONDecodeError: kinds.append(['partial'])
        states.append({'path': str(p), 'bytes': len(raw), 'kinds': kinds})
    return {'at_ns': time.monotonic_ns(), 'label': label, 'states': states}


def capture(root):
    data = json.load(sys.stdin)
    row = observe(root, data['hook_event_name'], data.get('transcript_path'))
    row['transcript_path'] = data.get('transcript_path')
    (root / 'observations' / f'{row["at_ns"]}-{os.getpid()}.json').write_text(json.dumps(row), encoding='utf-8')


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--codex', default='codex'); ap.add_argument('--capture', action='store_true')
    a = ap.parse_args(); root = a.work.resolve()
    if a.capture: capture(root); return
    root.mkdir(parents=True, exist_ok=False)
    for name in ('home', 'cwd', 'observations'): (root / name).mkdir()
    samples = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args): pass
        def do_POST(self):
            self.rfile.read(int(self.headers['Content-Length']))
            self.send_response(200); self.send_header('Content-Type', 'text/event-stream'); self.end_headers()

            def send(obj):
                self.wfile.write(('event: ' + obj['type'] + '\ndata: ' + json.dumps(obj) + '\n\n').encode()); self.wfile.flush()

            send({'type': 'response.created', 'response': {'id': 'g2-probe'}})
            send({'type': 'response.output_item.done', 'item': {'type': 'message', 'id': 'mid', 'role': 'assistant',
                  'phase': 'commentary', 'content': [{'type': 'output_text', 'text': 'Synthetic intermediate observation.'}]}})
            time.sleep(.4)
            samples.append(observe(root, 'mid-response-no-lifecycle-event'))
            send({'type': 'response.output_item.done', 'item': {'type': 'message', 'id': 'final', 'role': 'assistant',
                  'phase': 'final_answer', 'content': [{'type': 'output_text', 'text': 'Synthetic probe complete.'}]}})
            send({'type': 'response.completed', 'response': {'id': 'g2-probe', 'usage': {'input_tokens': 0, 'output_tokens': 0, 'total_tokens': 0}}})

    server = http.server.HTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    home = root / 'home'
    (home / 'config.toml').write_text(f'''model = "g2-mock"
model_provider = "g2mock"
approval_policy = "never"
sandbox_mode = "read-only"
[model_providers.g2mock]
name = "g2 localhost probe"
base_url = "http://127.0.0.1:{server.server_port}/v1"
wire_api = "responses"
requires_openai_auth = false
''', encoding='utf-8')
    command = shlex.join([sys.executable, str(Path(__file__).resolve()), '--capture', '--work', str(root)])
    events = ('SessionStart', 'UserPromptSubmit', 'PreToolUse', 'PostToolUse', 'Stop', 'SubagentStop', 'PreCompact', 'SessionEnd')
    (home / 'hooks.json').write_text(json.dumps({'hooks': {name: [{'hooks': [{'type': 'command', 'command': command}]}] for name in events}}), encoding='utf-8')
    # Only this child sees an isolated home. No existing settings or auth copied.
    env = {k: v for k, v in os.environ.items() if not any(x in k for x in ('API_KEY', 'TOKEN', 'SECRET', 'AUTH'))}
    env.update(CODEX_HOME=str(home), CODEX_SQLITE_HOME=str(home))
    cmd = [a.codex, 'exec', '--skip-git-repo-check', '--dangerously-bypass-hook-trust', '-C', str(root / 'cwd'), 'Return the synthetic response. Do not call tools.']
    try:
        proc = subprocess.run(cmd, env=env, capture_output=True, timeout=60)
    finally:
        server.shutdown(); server.server_close(); thread.join()
    (root / 'stdout.txt').write_bytes(proc.stdout); (root / 'stderr.txt').write_bytes(proc.stderr)
    samples.extend(json.loads(p.read_text()) for p in (root / 'observations').glob('*.json'))
    samples.append(observe(root, 'process-exit'))
    result = {'process_rc': proc.returncode, 'command': cmd,
              'version': subprocess.check_output([a.codex, '--version'], text=True).strip(),
              'boundary': 'real installed CLI, synthetic localhost model, isolated settings',
              'observations': sorted(samples, key=lambda r: r['at_ns'])}
    (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))


if __name__ == '__main__': main()

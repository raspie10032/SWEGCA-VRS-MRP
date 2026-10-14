#!/usr/bin/env python3
"""Real installed Codex, isolated settings, localhost synthetic subagent tool call."""
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


def capture(root, daemon):
    j = json.load(sys.stdin)
    ancestors = []
    pid = os.getpid()
    transcript = j.get('transcript_path')
    while pid > 1:
        try:
            raw = Path(f'/proc/{pid}/stat').read_text()
            fields = raw[raw.rfind(')') + 2:].split()
            row = dict(pid=pid, parent=int(fields[1]), birth=fields[19], comm=raw[raw.find('(')+1:raw.rfind(')')], writers=[])
            for fd in Path(f'/proc/{pid}/fd').iterdir():
                try:
                    if transcript and os.readlink(fd) == transcript:
                        flags = next(x.split()[1] for x in Path(f'/proc/{pid}/fdinfo/{fd.name}').read_text().splitlines() if x.startswith('flags:'))
                        row['writers'].append(dict(fd=fd.name, flags=flags))
                except (FileNotFoundError, PermissionError): pass
            ancestors.append(row); pid = row['parent']
        except FileNotFoundError: break
    r = subprocess.run([str(daemon), '--writer-hook', str(root / 'writers'), '--host', 'codex'], input=json.dumps(j).encode(), capture_output=True)
    row = dict(event=j, ancestors=ancestors, writer_hook_rc=r.returncode, writer_hook_stderr=r.stderr.decode(errors='replace'))
    (root / 'observations' / f'{time.monotonic_ns()}-{os.getpid()}.json').write_text(json.dumps(row, indent=2))
    # Hook output shape valid for every registered command event.
    print('{}')


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--daemon', type=Path, required=True); ap.add_argument('--capture', action='store_true')
    ap.add_argument('--codex', default='codex'); a = ap.parse_args()
    root = a.work.resolve(); daemon = a.daemon.resolve()
    if a.capture: capture(root, daemon); return
    root.mkdir(parents=True, exist_ok=False)
    for part in ('home', 'cwd', 'observations', 'writers'): (root / part).mkdir()
    requests = []
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args): pass
        def do_POST(self):
            data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            request_id = len(requests); requests.append(data)
            (root / f'request-{request_id}.json').write_text(json.dumps(data, indent=2))
            tools = [t.get('name') for t in data.get('tools', [])]
            self.send_response(200); self.send_header('Content-Type', 'text/event-stream'); self.end_headers()
            def send(j):
                self.wfile.write(('event: ' + j['type'] + '\ndata: ' + json.dumps(j) + '\n\n').encode()); self.wfile.flush()
            send(dict(type='response.created', response=dict(id=f'g3-{request_id}')))
            spawn = 'spawn_agent' if 'multi_agent_v1' in tools else None
            # First root response asks the installed CLI to spawn a real child
            # session; the model for both root and child is this local fixture.
            if request_id == 0 and spawn:
                item = dict(type='function_call', namespace='multi_agent_v1', name=spawn, call_id='g3-spawn', arguments=json.dumps(dict(message='G3_CHILD: Return a synthetic final response without tools.')))
            else:
                outcomes = {x.get('call_id'): x.get('output') for x in data.get('input', []) if x.get('type') == 'function_call_output'}
                if 'g3-spawn' in outcomes and 'g3-wait' not in outcomes:
                    target = json.loads(outcomes['g3-spawn'])['agent_id']
                    item = dict(type='function_call', namespace='multi_agent_v1', name='wait_agent', call_id='g3-wait', arguments=json.dumps(dict(targets=[target], timeout_ms=10000)))
                else:
                    item = dict(type='message', id=f'final-{request_id}', role='assistant', phase='final_answer', content=[dict(type='output_text', text='G3 synthetic probe complete.')])
            send(dict(type='response.output_item.done', item=item))
            send(dict(type='response.completed', response=dict(id=f'g3-{request_id}', usage=dict(input_tokens=0, output_tokens=0, total_tokens=0))))
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    home = root / 'home'
    (home / 'config.toml').write_text(f'''model = "g3-mock"
model_provider = "g3mock"
approval_policy = "never"
sandbox_mode = "read-only"
[features]
multi_agent = true
[model_providers.g3mock]
name = "g3 localhost probe"
base_url = "http://127.0.0.1:{server.server_port}/v1"
wire_api = "responses"
requires_openai_auth = false
''')
    command = shlex.join([sys.executable, str(Path(__file__).resolve()), '--capture', '--work', str(root), '--daemon', str(daemon)])
    events = ('SessionStart', 'UserPromptSubmit', 'SubagentStart', 'SubagentStop', 'Stop', 'SessionEnd')
    (home / 'hooks.json').write_text(json.dumps({'hooks': {e: [{'hooks': [{'type': 'command', 'command': command}]}] for e in events}}))
    env = {k: v for k, v in os.environ.items() if not any(x in k for x in ('API_KEY', 'TOKEN', 'SECRET', 'AUTH'))}
    env.update(CODEX_HOME=str(home), CODEX_SQLITE_HOME=str(home))
    cmd = [a.codex, 'exec', '--skip-git-repo-check', '--dangerously-bypass-hook-trust', '-C', str(root / 'cwd'), 'Execute the synthetic subagent call and return.']
    try: proc = subprocess.run(cmd, env=env, capture_output=True, timeout=90)
    finally: server.shutdown(); server.server_close(); thread.join()
    (root / 'stdout.txt').write_bytes(proc.stdout); (root / 'stderr.txt').write_bytes(proc.stderr)
    result = dict(process_rc=proc.returncode, version=subprocess.check_output([a.codex, '--version'], text=True).strip(),
        requests=len(requests), tool_names=[[t.get('name') for t in r.get('tools', [])] for r in requests],
        observations=[json.loads(p.read_text()) for p in sorted((root / 'observations').glob('*.json'))],
        rollouts=[str(p) for p in (home / 'sessions').rglob('*.jsonl')], markers=[p.read_text() for p in (root / 'writers').glob('*.mark')],
        boundary='installed CLI + localhost synthetic model + isolated configuration, no remote model or credentials')
    (root / 'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))
if __name__ == '__main__': main()

#!/usr/bin/env python3
"""G3 deterministic lost-observation injections. Delays are never injection points."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import statistics
import subprocess
import sys
import time
from kwov_test import line, cover, HandleSamples


def rpc(endpoint, request):
    if os.name == 'nt': stream = open(endpoint, 'r+b', buffering=0); send = stream.write; recv = stream.read
    else:
        stream = socket.socket(socket.AF_UNIX); stream.settimeout(30); stream.connect('\0'+endpoint[1:]); send = stream.sendall; recv = stream.recv
    try:
        send(json.dumps(request).encode()+b'\n'); raw = b''
        while b'\n' not in raw:
            chunk = recv(65536)
            if not chunk: raise AssertionError('incomplete daemon reply')
            raw += chunk
        result = json.loads(raw.split(b'\n')[0]); assert result.get('ok'), result
        return result
    finally: stream.close()


def publish(daemon, root, path, pid):
    began = time.perf_counter_ns()
    p = subprocess.run([str(daemon), '--writer-root', str(root), '--writer-path', str(path), '--writer-pid', str(pid)], capture_output=True, timeout=20)
    assert p.returncode == 0, p.stderr
    assert not p.stdout
    return (time.perf_counter_ns()-began)/1000


def validate(files, projects, data):
    count = 0
    for p in files:
        expected, offset = {}, 0
        for raw in p.read_bytes().splitlines(keepends=True):
            j = json.loads(raw)
            if j.get('type') == 'user': expected[offset] = (raw[:-1], j['message']['content'])
            offset += len(raw)
        actual = set(); converted = data / 'cc_rollout' / p.relative_to(projects)
        if expected:
            for raw in converted.read_bytes().splitlines():
                row = json.loads(re.sub(rb'("ordinal":)0*(\d+)', rb'\1\2', raw, count=1)); payload = row['payload']
                if payload.get('type') != 'message': continue
                c = payload['cc']; source, text = expected[c['o']]
                assert c['n'] == len(source) and c['h'] == hashlib.sha256(source).hexdigest()
                assert payload['content'][0]['text'] == text and c['o'] not in actual
                actual.add(c['o']); count += 1
        assert actual == set(expected), (p, expected.keys(), actual)
    return count


def writer_child():
    # Kept alive by its stdin pipe; every command ACK follows write completion.
    for raw in sys.stdin:
        j = json.loads(raw)
        if j['op'] == 'append':
            with open(j['path'], 'ab') as f: f.write(line(j['id'], j['number']))
        print('{}', flush=True)


def run_case(daemon, work, count):
    work.mkdir(parents=True); projects = work/'projects'; data = work/'data'; writers = work/'writers'
    projects.mkdir(); writers.mkdir()
    if os.name != 'nt':
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        if soft > 4096:
            resource.setrlimit(resource.RLIMIT_NOFILE, (4096, hard))
        assert resource.getrlimit(resource.RLIMIT_NOFILE)[0] == 4096
    files = []
    for i in range(count):
        p = projects/f'{i:05}.jsonl'; p.write_bytes(line(i, 0) if i < 32 else b'{"type":"progress"}\n'); files.append(p)
    child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '--writer-child'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    times = [publish(daemon, writers, p, os.getpid()) for p in files[:32]]
    times.append(publish(daemon, writers, files[-1], child.pid))
    # All W facts exist before initial conversion, so processing cannot retire
    # a live writer merely because cover has reached EOF.
    env = dict(os.environ, VRS_CC_TEST_METRICS='1', VRS_CC_TEST_DROP_WATCH='1')
    endpoint = ('\\\\.\\pipe\\vrs-g3-' if os.name == 'nt' else '@vrs-g3-') + str(os.getpid())
    logpath = work/'daemon.log'
    with logpath.open('wb') as log:
        proc = subprocess.Popen([str(daemon), '--projects', str(projects), '--data', str(data), '--endpoint', endpoint,
                                 '--writer-markers', str(writers), '--workers', '6', '--connections', '4'], env=env, stderr=log, stdout=subprocess.DEVNULL)
        monitor = HandleSamples(proc)
        def logs(): return logpath.read_text(errors='replace')
        def wait(predicate, label):
            end = time.monotonic()+120
            while time.monotonic() < end:
                assert proc.poll() is None, logs()[-4000:]
                if predicate(): return
                time.sleep(.01) # Only observing completion, never arranging a race.
            raise AssertionError(label+'\n'+logs()[-4000:])
        def covered(): return all(cover(data/'cover'/(str(p.relative_to(projects))+'.cv')) == p.stat().st_size for p in files)
        try:
            wait(lambda: 'VRS_TEST_OPEN' in logs() and 'VRS_TEST_WATCH_READY' in logs() and covered(), 'startup')
            assert rpc(endpoint, dict(op='test_watch'))['drop'] is True
            assert len(list(writers.glob('*.mark'))) == 33
            # 1. Append between hooks: no new W or M is published.
            for i,p in enumerate(files[:32]):
                with p.open('ab') as f: f.write(line(i, 1))
            # 2. Actual writer appends, ACKs, then is forcibly terminated. Its
            # established W must survive; there is deliberately no later hook.
            child.stdin.write(json.dumps(dict(op='append',path=str(files[-1]),id=900,number=1))+'\n'); child.stdin.flush()
            assert child.stdout.readline() == '{}\n'
            child.kill(); child.wait(timeout=10)
            assert cover(data/'cover'/(files[-1].name+'.cv')) < files[-1].stat().st_size
            # Four new sources keep the required totals 68/4132, Δ=37.
            new = projects/'new'; new.mkdir()
            parent = new/'session.jsonl'; parent.write_bytes(line(100,1)); files.append(parent)
            # This source is known only through its parent's W. There is no W
            # for the nested file and every directory notification is lost.
            sub = new/'session'/'subagents'/'child.jsonl'; sub.parent.mkdir(parents=True); sub.write_bytes(line(101,1)); files.append(sub)
            times.append(publish(daemon,writers,parent,os.getpid()))
            # 3. Append predates W; its creation event is also deterministically lost.
            pre = new/'pre-marker.jsonl'; pre.write_bytes(line(102,1)); files.append(pre)
            times.append(publish(daemon,writers,pre,os.getpid()))
            # 4. PID reuse: current live PID, different raw creation time. Must
            # be treated as the dead old identity, retained until fully covered.
            reused = new/'reused.jsonl'; reused.write_bytes(line(103,1)); files.append(reused)
            times.append(publish(daemon,writers,reused,os.getpid()))
            marker = next(p for p in writers.glob('*.mark') if p.read_text().endswith(str(reused).replace('\\','/')))
            fields = marker.read_text().split('\n',4); fields[2] = str(int(fields[2])+1)
            fixture = '\n'.join(fields).encode(); marker.unlink(); reused_marker = writers/(hashlib.sha256(fixture).hexdigest()+'.mark'); reused_marker.write_bytes(fixture)
            assert not covered()
            observed = rpc(endpoint,dict(op='test_overflow'))
            assert observed['marks'] == 0 and observed['writers'] <= 36 and observed['sources'] <= 37, observed
            assert observed['writers'] == 36 and observed['sources'] == 37, observed
            wait(covered,'four boundary cases recovered')
            first = validate(files,projects,data)
            # lib-specific overflow takes its host callback; cover==EOF must
            # still schedule the C(T) disk tail. No restore call is permitted.
            rpc(endpoint,dict(op='test_overflow',origin='lib'))
            # An additional lost append is recovered by lib overflow alone.
            with files[0].open('ab') as f: f.write(line(200,2))
            rpc(endpoint,dict(op='test_overflow',origin='lib'))
            wait(covered,'lib callback recovery')
            rpc(endpoint,dict(op='test_overflow')) # retire both dead identities after cover
            assert not reused_marker.exists()
            for p in writers.glob('*.mark'):
                assert str(child.pid) != p.read_text().split('\n')[1]
            assert len(list(writers.glob('*.mark'))) == 34
            assert validate(files,projects,data) == first+1
            # Missing source at SessionStart is legal; its later complete line
            # is found by W without requiring a source creation notification.
            absent = new/'unborn.jsonl'; publish(daemon,writers,absent,os.getpid())
            rpc(endpoint,dict(op='test_overflow'))
            absent.write_bytes(line(300,3)); files.append(absent)
            rpc(endpoint,dict(op='test_overflow')); wait(covered,'unborn source')
            assert validate(files,projects,data) == first+2
            # A session-folder link back to the whole history must not widen D(w).
            linked = new/'linked.jsonl'; linked.write_bytes(line(301,4)); files.append(linked)
            folder=linked.with_suffix('')
            if os.name=='nt':
                link=subprocess.run(['cmd','/c','mklink','/J',str(folder),str(projects)],capture_output=True)
                assert link.returncode==0,(link.stdout,link.stderr)
            else: folder.symlink_to(projects,target_is_directory=True)
            publish(daemon,writers,linked,os.getpid())
            link_scan=rpc(endpoint,dict(op='test_overflow'))
            assert link_scan['writers']==36 and link_scan['sources']==37,link_scan
            wait(covered,'linked session root excluded')
            assert validate(files,projects,data)==first+3
            errors=[r for r in logs().splitlines() if r.startswith(('conversion:','watch:','writer retained:'))]
            assert not errors, errors
            rows = [dict(origin=o,marks=int(m),writers=int(w),sources=int(s)) for o,m,w,s in re.findall(r'VRS_TEST_WITNESSES origin=(\w+) marks=(\d+) writers=(\d+) sources=(\d+)',logs())]
            assert any(r['origin']=='lib' for r in rows)
            result=dict(pass_=True,initial_files=count,comparison_files=count+4,changed_files=37,main=observed,scans=rows,
                boundary_cases=['between-hooks-append','append-kill-overflow','append-before-marker-lost-creation','pid-reuse'],
                extra_cases=['session-subagent','unborn-source','lib-callback','session-root-link'],messages=first+3,restore_calls=0,
                publish_median_us=statistics.median(times),publish_max_us=max(times))
        finally:
            handles = monitor.finish(work)
            if child.poll() is None: child.kill(); child.wait()
            proc.terminate()
            try: proc.wait(timeout=30)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    result['handles']=handles; result['pass']=result.pop('pass_') and handles['pass']; (work/'result.json').write_text(json.dumps(result,indent=2)+'\n'); return result


def hook_child(daemon, work, host):
    work.mkdir(parents=True)
    source=work/'session.jsonl'; source.write_bytes(line(0,0))
    env=dict(os.environ)
    if host=='claude':
        home=work/'claude'; (home/'sessions').mkdir(parents=True)
        (home/'sessions'/f'{os.getpid()}.json').write_text(json.dumps(dict(sessionId='g3-hook-session')))
        env['CLAUDE_CONFIG_DIR']=str(home)
    observed=[]
    # The actual writer keeps this writable descriptor open while its hook runs.
    with source.open('ab'):
        for event in ('SessionStart','UserPromptSubmit'):
            began=time.perf_counter_ns()
            r=subprocess.run([str(daemon),'--writer-hook',str(work/'writers'),'--host',host],
                input=json.dumps(dict(transcript_path=str(source),session_id='g3-hook-session',hook_event_name=event)).encode(),env=env,capture_output=True,timeout=20)
            assert r.returncode==0,(host,r.stderr)
            observed.append((time.perf_counter_ns()-began)/1000)
    marks=list((work/'writers').glob('*.mark')); assert len(marks)==1
    assert int(marks[0].read_text().split('\n')[1])==os.getpid()
    # Invalid transcript/ancestry is a visible nonzero hook result.
    r=subprocess.run([str(daemon),'--writer-hook',str(work/'writers'),'--host',host],input=b'{}',env=env,capture_output=True)
    assert r.returncode!=0
    print(json.dumps(dict(host=host,pass_=True,writer_pid=os.getpid(),invocations_us=observed,invalid_hook_rc=r.returncode)))


def retirement(daemon,work):
    work.mkdir(parents=True); projects=work/'projects'; projects.mkdir(); data=work/'data'; writers=work/'writers'
    source=projects/'retire.jsonl'; source.write_bytes(line(0,0))
    child=subprocess.Popen([sys.executable,str(Path(__file__).resolve()),'--writer-child'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    publish(daemon,writers,source,child.pid)
    endpoint=('\\\\.\\pipe\\vrs-g3-retire-' if os.name=='nt' else '@vrs-g3-retire-')+str(os.getpid())
    logpath=work/'daemon.log'
    with logpath.open('wb') as log:
        proc=subprocess.Popen([str(daemon),'--projects',str(projects),'--data',str(data),'--endpoint',endpoint,'--writer-markers',str(writers)],
            env=dict(os.environ,VRS_CC_TEST_METRICS='1'),stderr=log,stdout=subprocess.DEVNULL)
        def wait(pred,label):
            end=time.monotonic()+120
            while time.monotonic()<end:
                assert proc.poll() is None,logpath.read_text()
                if pred():return
                time.sleep(.01)
            raise AssertionError(label+' '+logpath.read_text())
        try:
            wait(lambda:'VRS_TEST_OPEN' in logpath.read_text() and cover(data/'cover'/'retire.jsonl.cv')==source.stat().st_size,'initial')
            assert len(list(writers.glob('*.mark')))==1
            child.stdin.write(json.dumps(dict(op='append',path=str(source),id=1,number=1))+'\n');child.stdin.flush()
            assert child.stdout.readline()=='{}\n'
            child.kill();child.wait(timeout=10)
            wait(lambda:not list(writers.glob('*.mark')),'periodic retirement')
            assert cover(data/'cover'/'retire.jsonl.cv')==source.stat().st_size
            assert validate([source],projects,data)==2
            assert 'conversion:' not in logpath.read_text() and 'watch:' not in logpath.read_text()
            return dict(pass_=True,remaining_writers=0,messages=2)
        finally:
            if child.poll() is None:child.kill();child.wait()
            proc.terminate()
            try:proc.wait(timeout=30)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()


def run_suite(daemon,work):
    daemon=daemon.resolve()
    hooks=[]
    for host in ('claude','codex'):
        p=subprocess.run([sys.executable,str(Path(__file__).resolve()),'--hook-child','--host',host,'--daemon',str(daemon),'--work',str(work/('hook-'+host))],capture_output=True,timeout=60)
        assert p.returncode==0,p.stderr
        hooks.append(json.loads(p.stdout))
    rows=[run_case(daemon,work/str(n),n) for n in (64,4128)]
    assert rows[0]['main']==rows[1]['main'],rows
    retired = retirement(daemon,work/'retirement')
    return dict(pass_=all(r['pass'] for r in rows) and all(h['pass_'] for h in hooks) and retired['pass_'],cases=rows,hooks=hooks,periodic_retirement=retired)

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--writer-child',action='store_true');ap.add_argument('--daemon',type=Path);ap.add_argument('--work',type=Path)
    ap.add_argument('--hook-child',action='store_true');ap.add_argument('--host',choices=['claude','codex'])
    a=ap.parse_args()
    if a.writer_child: writer_child()
    elif a.hook_child:hook_child(a.daemon.resolve(),a.work.resolve(),a.host)
    else: print(json.dumps(run_suite(a.daemon,a.work.resolve())))

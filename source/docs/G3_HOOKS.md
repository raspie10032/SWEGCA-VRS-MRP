# G3 격리 훅 사용법 / Isolated writer hooks

이 문서는 설치 예시다. 실제 `~/.claude/hooks`, `settings.json`, `~/.claude.json`,
Codex 사용자 설정을 이 작업에서 수정하지 않는다. 교체 시 사용자가 승인한
설정 폴더에 적용해야 한다. 훅 없는 작성자의 넘침 복구는 계약 밖이고 시작 시
전체 스캔은 그대로다.

Start a daemon with explicit roots (all paths refer to your isolated fixture):

```sh
vrs_ccd --projects /isolated/projects --data /isolated/data \
  --endpoint @vrs-g3 --writer-markers /isolated/writers \
  --change-markers /isolated/marks
```

Windows endpoint example: `\\.\pipe\vrs-g3`. The same executable accepts UTF-8
hook JSON on stdin; the Windows command line uses its native wide entry point.
The default W root is `<data>/writer-markers`, M is `<data>/change-markers`.
Each publisher and the daemon must use the **same absolute W root**.

Claude lifecycle command:

```sh
/path/to/vrs_ccd --writer-hook /isolated/writers --host claude
```

Use this command for **SessionStart and UserPromptSubmit**. It follows the hook's
ancestor chain and requires the ancestor's `.claude/sessions/<pid>.json` sessionId
to match the hook's session_id. `CLAUDE_CONFIG_DIR`, if set, selects the isolated
Claude configuration root. It never reads Claude settings or credentials.

Codex lifecycle command:

```sh
/path/to/vrs_ccd --writer-hook /isolated/writers --host codex
```

Use this command for **SessionStart, UserPromptSubmit, and SubagentStart**.
Installed Codex0.153.4 emitted SubagentStart, not a separate SessionStart, for a
child rollout. Its UserPromptSubmit also carried the child transcript path even
though session_id referred to the parent. Selection therefore uses the exact
rollout path held open for writing by an ancestor, not session_id or process name.
The local model probe creates a real CLI child but uses no remote model service.

Minimal isolated hook object (replace COMMAND with the appropriate command above;
quote each Windows path containing spaces):

```json
{
  "hooks": {
    "SessionStart": [{"hooks": [{"type": "command", "command": "COMMAND"}]}],
    "UserPromptSubmit": [{"hooks": [{"type": "command", "command": "COMMAND"}]}],
    "SubagentStart": [{"hooks": [{"type": "command", "command": "COMMAND"}]}]
  }
}
```

The extra SubagentStart entry is for Codex. Claude's dependent subagent files are
recovered under `<session>/subagents` through that session's W; a distinct writer
process that outlives the parent needs its own W. Successful hooks are silent.
Invalid/missing transcript, no matching ancestor, permission failure, or publication
failure returns nonzero. A host that continues after that failure does not meet H.
No fallback scan hides the failure.

W survives successful conversion while its writer is alive. The daemon checks W
on publication, overflow, and its normal one-second maintenance opportunity.
After the writer exits and every dependent file is covered, the daemon removes W
and checks source sizes again. Users and hooks must not delete W themselves.
A malformed or unreadable marker is an observable error, not proof of completion.

M remains the G2 post-append contract. Its commands are unchanged:

```sh
vrs_ccd --mark-root /isolated/marks --mark-path /isolated/projects/session.jsonl
vrs_ccd --mark-hook /isolated/marks
```

Standalone lib users may set `vrs_opts.change_markers` and `writer_markers`; otherwise
those roots are `<sessions_dir>.marks` and `<sessions_dir>.writers`. Overflow
visits only these sets. W retirement belongs to the lifecycle daemon, so standalone
users need a companion managing those witness lifetimes or a `recover(context)`
callback to their daemon, as vrs_ccd supplies. The library does not retire W or
silently recover uninstrumented history. `vrs_refresh(h, path)` schedules exactly
that existing file's disk tail; it does not change assembly or selection rules.
Rebuild all static-library clients with the extended `vrs_opts` declaration.

Reproduce local validation (new output paths each run):

```sh
taskset -c 0-11 python3 src/build_native.py --platform linux --build .g3/rebuild
taskset -c 0-11 python3 tools/linux_n1.py --binary .g3/rebuild/vrs_cli --out .g3/recheck-n1
taskset -c 0-11 python3 tools/kwov_test.py --daemon .g3/rebuild/vrs_ccd --work .g3/recheck-kwov
taskset -c 0-11 python3 tools/g3_hooks_probe.py --daemon .g3/rebuild/vrs_ccd --work .g3/recheck-hooks
```

H2 integrates I's bounded handle leases. The large fixtures run with Linux soft
NOFILE=4096; the tests never raise that limit. The original, M-marker, and W-writer
fixtures all retain the fixed observed handle bound of 512. Windows results come
from the fixed relay; Linux results are separate evidence.

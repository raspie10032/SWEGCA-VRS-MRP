# Claude Code → libvrs/lib7 L3 구현

이 디렉터리는 참조 lib7을 수정하지 않고 변환기, 상주 데몬, 표준 라이브러리만
쓰는 Python stdio MCP 다리를 구현한다. 현재 판정과 알려진 정의 충돌은
[`../REPORT4.md`](../REPORT4.md), 실행 결과는 [`../tests/task4/`](../tests/task4/)에 있다.
실제 Claude/Windows 통합 승인이나 라이브 교체를 뜻하지 않는다.

## Linux 빌드·시험

작업 루트에서 실행한다. Python 3.10 이상과 C++20 g++가 필요하다.

```bash
taskset -c 0-11 bash src/build.sh
taskset -c 0-11 bash src/tests/run_u.sh
taskset -c 0-11 python3 -B src/tests/static_check.py
```

`run_u.sh`는 다시 빌드한다. 시험 중 하나라도 실패하면 종료값 1을 돌려준다.
결과를 통과로 바꾸지 않는다. 매 실행의 원본 사본·변환 파일·색인·데몬 로그는
`src/tests/u-*/`에 남고, 최신 경로는 `u_results.txt` 마지막 줄에 기록된다.
실행 파일은 `src/build/vrs_ccd`, 단위시험 도구는 `src/build/u_native`다.

```bash
taskset -c 0-11 src/build/vrs_ccd \
  --projects "$PWD/sample/projects" \
  --data "$PWD/src/demo-data" \
  --endpoint @vrs-lib-demo \
  --workers 6 --connections 16 --timeout-ms 5000
```

자료는 `<data>/cc_rollout/<projects에서의 상대 경로>`와 `<data>/idx`에 생긴다.
변환 완료 사실은 lib의 sessions 루트 밖인 `<data>/cover/<상대 경로>.cv`에 둔다.
파일마다 64슬롯×8바이트 LE(512바이트)이며, 일꾼은 0…W−1, 요청 스레드는
W…W+K−1 슬롯을 사용한다. `--workers + --connections`는 64 이하여야 한다.
Linux 소켓 이름은 `@`로 시작하는 abstract Unix 주소다. 파일명 소켓은 받지 않는다.
종료는 해당 PID에 `kill -TERM PID`를 보낸다. Linux 요청은 JSON 한 줄/응답 한 줄이다.

```json
{"op":"restore","transcript":"/absolute/projects/project/session.jsonl","budget":48000}
{"op":"restore","transcript":"/absolute/projects/project/session.jsonl","start":"1234:0","budget":4000}
{"op":"restore","transcript":"/absolute/projects/project/session.jsonl","start":"1234:0:3600","budget":4000}
{"op":"read","path":"/absolute/projects/project/session.jsonl","start":0,"end":1000,"budget":48000}
{"op":"diag_assemble","transcript":"/absolute/projects/project/session.jsonl"}
```

`restore`의 `text`는 실제 MCP 본문이다. `items`, `boundary`, `next`, `utf16`은
시험용 원본 대조에도 쓸 수 있다. 압축이 없으면 표지와 빈 `items`를 돌려준다.
선택 범위는 lib7의 기본 조립 예산 258400으로 고정하고 `budget`은 출력 쪽만 제한한다.
`read`는 지정한 UTF-8 범위를 그대로 복호화하여 돌려준다. `bytes.encode('utf-8')`의
SHA-256이 `sha256`이다. UTF-8 중간 절단·비UTF-8 입력은 오류이며 대체 문자로 바꾸지 않는다.
허용 루트는 projects와 명시적으로 반복한 `--read-root ROOT`다.

## MCP 다리

Claude가 `python -u /absolute/src/bridge/vrs_lib_bridge.py`를 실행하도록 시험 설정에
등록한다. 다음은 설정 예이며 설치된 설정 파일을 수정하지 않는다.

```json
{
  "mcpServers": {
    "vrs-lib": {
      "command": "python3",
      "args": ["-u", "/absolute/vrscc/src/bridge/vrs_lib_bridge.py"],
      "env": {
        "VRS_CC_DAEMON": "/absolute/vrscc/src/build/vrs_ccd",
        "VRS_CC_PROJECTS": "/absolute/.claude/projects",
        "VRS_CC_DATA": "/absolute/vrs-lib-data",
        "VRS_CC_ENDPOINT": "@vrs-lib-demo"
      }
    }
  }
}
```

도구는 `vrs_restore(budget?, start?)`, `vrs_read(path, start, end, budget?)` 두 개다.
각 도구의 `_meta["anthropic/maxResultSizeChars"]`는 100000이다.
상한은 결과 글 전체의 UTF-16 단위 수이며 기본 48000, 최대 95000이다.
`vrs_restore`는 항목이 쪽보다 크면 코드 포인트 경계에서 나눈다. `next`는 `o:k:c`이며
`c`는 해당 항목 본문 안의 UTF-16 위치다. `start=o:k`도 받으며 `c=0`을 뜻한다.
조각 머리의 `이어짐 c=...`, 원본 머리, 쪽 표지와 개행까지 상한에 포함한다.
시험용 `items`의 `text`는 해당 조각이고 `c`가 시작 위치다. 같은 `(o,k)`의 조각을
`c` 순서로 붙이면 원래 본문이다. 서로게이트 쌍 중간의 start는 오류다.
표지·머리·다음 코드 포인트를 담을 수 없는 budget은 명시적으로 거부한다.
`vrs_read`는 기존대로 줄 경계에서 나누므로 한 줄이 상한보다 크면 오류다.

과제 4는 `cc_skip`을 제거하고 C2′/C2″의 바이트 사상을 복원했다. 0출력 줄의
완료 사실은 C3′ 덮개에만 남긴다. 먼저 덮개 max를 읽고 C(T)의 끝을 읽으며,
출력 위치 쓰기가 완료된 뒤 실행 스레드 자신의 슬롯을 쓴다. 변경 없는 완전한
입력의 재변환 꼬리는 0바이트다. U8′ 표본 넷에서 과제 2 출력과 전체 바이트가 같다.
KU6의 `o:k:c` 쪽 나누기는 유지한다. 과제 3의 `cc_skip` 포함 data는 재사용하지
말고 새 data 경로에서 원본으로 다시 구축한다. 자동 마이그레이션은 제공하지 않는다.
시험 기준·측정·Windows 인계와 남은 범위는 `../REPORT4.md`에 기록한다.

환경 변수:

| 변수 | 기본값/의미 |
|---|---|
| `VRS_CC_DAEMON` | `src/build/vrs_ccd[.exe]` |
| `VRS_CC_PROJECTS` | `~/.claude/projects` |
| `VRS_CC_DATA` | `~/.claude/vrs-lib` |
| `VRS_CC_ENDPOINT` | Linux `@vrs-lib-<uid>`, Windows `\\.\pipe\vrs-lib-<사용자>` |
| `VRS_CC_READ_ROOTS` | 추가 read 루트들, OS의 경로 목록 구분자로 구분 |
| `VRS_CC_TEST_SESSION_FILE` | **시험 전용** 세션 기록 파일; pid/procStart 검사는 그대로 수행 |
| `VRS_CC_TEST_METRICS` | **시험 전용**; 설정하면 데몬 내부 복원 시간·시작 시간·색인 로드 수를 stderr에 기록. 도구 응답에는 넣지 않음 |

다리는 부모 1–4단에서 처음 발견한 `~/.claude/sessions/<pid>.json`을 사용하며,
기록의 pid·procStart와 실제 생성 시각을 대조한다. 호출마다 다시 읽으므로
`/clear`로 바뀐 sessionId를 따른다. 찾지 못하면 오류다. 최신 파일 추측은 없다.
데몬 연결이 안 되면 분리 실행하고 연결을 최대 5초 재시도한다.

## Windows 인계

이 환경에서는 Windows 빌드·실행을 검증하지 않았다. 같은 파일의 `_WIN32` 분기는
Unicode 경로, 위치 지정 파일 I/O, Toolhelp32/GetProcessTimes, 상주 파이프 인스턴스,
`vp_watch_tree`를 사용한다. MinGW g++ 16 UCRT 예시 명령은 다음과 같다.

```text
g++ -O2 -std=c++20 -pthread -municode -D_WIN32_WINNT=0x0602 -DWINVER=0x0602 -ffp-contract=off -I ref -I ref/swegca_cpp -I src src/cc_conv.cpp src/vrs_ccd.cpp ref/vrs_lib.cpp ref/vrs_plat_win.cpp ref/swegca_cpp/swegca_architecture/evidence_rules.cpp ref/swegca_cpp/swegca_architecture/sha256.cpp ref/swegca_cpp/swegca_architecture/strong_types.cpp -lsynchronization -o src/build/vrs_ccd.exe
```

`src/build_windows.ps1 -Cxx <MinGW16-UCRT/bin/g++.exe> -Nm <MinGW16-UCRT/bin/nm.exe>`는
직접 목적 파일을 남기고 데몬·M1 시험 도구의 빌드와 `nm -u` 검사를 함께 한다.
출력은 `src/build-windows`이므로 다리를 띄울 때 `VRS_CC_DAEMON`을 그 경로로 지정한다.
설치나 라이브 설정 변경은 하지 않는다. 과제 2의 측정·한계·Windows 인계는 `REPORT2.md`를 본다.

Windows 파이프 기본 이름은 `\\.\pipe\vrs-lib-<사용자>`다. 첫 인스턴스의
`FILE_FLAG_FIRST_PIPE_INSTANCE`가 같은 이름의 중복 실행을 거부한다.
Claude 실제 부모 정체(U7), 파이프·감시 경합, 비ASCII 실행 인수, 목적 파일 정적
검사와 L4는 Windows 담당자가 이어서 검증해야 한다.

## English quick reference

Build with `taskset -c 0-11 bash src/build.sh`; run the sample suite with
`taskset -c 0-11 bash src/tests/run_u.sh`. The reference library is compiled unchanged.
The converter preserves only the documented speech and structural fields. Restore
always rereads and authenticates the original transcript bytes. The Python bridge
uses only the standard library and resolves the caller from its verified parent
process record. Run the bridge under an actual Claude process or use the explicitly
named TEST override in an isolated test. Task 4 removes `cc_skip` and preserves the
task 2 rollout bytes. Coverage facts live outside `cc_rollout`, in 64 little-endian
8-byte slots per file. Workers and connection threads own disjoint slots; their
combined count must not exceed 64. Keep the `o:k:c` continuation for large items.
Use a fresh data directory for stores previously built with task 3 `cc_skip`.
Check REPORT4.md for measured local acceptance and remaining Windows/Claude work.

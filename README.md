# vrs-lib 1.0.0

Claude Code(Windows)용 MCP 서버입니다. 컨텍스트 압축은 그대로 두고, 압축 뒤에 **압축 전 원문**(사용자 발화와 답)을 바이트 그대로 돌려주는 데만 씁니다.

- 엔진: libvrs lib7(동결본)을 Windows 네이티브로 옮긴 것입니다.
- `source/` 는 개발 저장소 태그 `v1.0.0`(커밋 `fc46464`)의 소스 전체입니다. `bin/vrs_ccd.exe` 는 이 소스로 빌드한 것입니다.

## 도구

| 도구 | 인자 | 하는 일 |
|---|---|---|
| `vrs_restore` | 없음 (`start`, `budget` 은 선택) | 이 세션의 마지막 압축 전 원문 발화·답을 돌려줍니다. 여러 쪽이면 결과의 `next` 를 `start` 로 넘겨 이어 받습니다. |
| `vrs_read` | `path`, `start`, `end` | 대화 기록 폴더와 `VRS_CC_READ_ROOTS`(`;` 로 구분)에 준 폴더 아래 파일의 정확한 바이트 범위를 SHA-256 과 함께 읽습니다. |

`NOT_READY` 는 색인을 준비하는 중이라는 뜻이지 원문이 없다는 뜻이 아닙니다. 몇 초 뒤 같은 인자로 다시 부르면 됩니다.

## 설치

요구 사항: Windows 10/11 x64, Claude Code, Git, CPython 3(시험한 판 3.11; Microsoft Store 의 `python` 스텁은 안 됨).

```bash
git clone https://github.com/raspie10032/vrs-lib.git vrs-lib
```

비공개 저장소라 클론하려면 이 저장소에 접근 권한이 있는 GitHub 계정으로 로그인돼 있어야 합니다(`gh auth login` 이나 Git Credential Manager).

```bash
cd vrs-lib
```

```bash
python install.py
```

- **클론한 폴더가 곧 설치 위치입니다.** 훅과 MCP 항목이 이 폴더를 가리키므로 지우거나 옮기지 마세요. 옮겼다면 `python install.py` 를 다시 실행합니다.
- 설치기가 하는 일:
  - `bin/vrs_ccd.exe`·다리·훅이 `SHA256SUMS.txt` 와 맞는지 확인합니다. 줄바꿈 변환 같은 손상이 있으면 멈춥니다.
  - `~/.claude/settings.json` 을 `settings.json.bak-vrslib-<시각>` 으로 백업합니다.
  - 그 파일의 vrs-lib 훅 세 개를 넣거나 바꿉니다: SessionStart 전체(쓰기 표지), SessionStart compact(안내), UserPromptSubmit(쓰기 표지).
  - MCP 서버 `vrs-lib` 를 사용자 범위로 등록합니다(`claude mcp add-json -s user`).
- `claude` CLI 는 PATH 에서 찾고, 없으면 데스크톱 앱에 든 것을 찾습니다. 그래도 못 찾으면 등록 명령만 출력합니다. `--claude <경로>` 로 직접 줄 수도 있습니다.
- 데이터 폴더는 기본 `%USERPROFILE%\.vrs-lib\data` 입니다. `--data <폴더>` 로 바꿉니다.
- 다시 실행하면 이전 vrs-lib 항목을 바꿔 넣고, 겹쳐 넣지 않습니다. 새로 여는 세션부터 적용됩니다.

**이미 다른 판의 데몬이 떠 있으면** 다리가 그 데몬에 붙습니다(파이프가 사용자마다 하나). 바꾸려면 세션을 모두 닫고 `Get-Process vrs_ccd | Stop-Process` 로 데몬을 끝낸 뒤 새 세션을 엽니다.

## 업데이트

```bash
git pull
```

```bash
python install.py
```

## 제거

```bash
python uninstall.py
```

- 훅과 MCP 항목을 지웁니다.
- 데이터 폴더와 클론한 폴더는 지우지 않습니다.
- 데몬은 따로 떠 있는 프로세스라 계속 돕니다. 끝내려면 `Get-Process vrs_ccd | Stop-Process` 를 실행합니다.

## 저장소 구성

| 경로 | 내용 |
|---|---|
| `bin/vrs_ccd.exe` | 데몬과 쓰기 표지 훅(`--writer-hook`)이 든 단일 실행 파일 |
| `bridge/vrs_lib_bridge.py` | MCP stdio 다리. 표준 라이브러리만 씁니다. MCP 초기화 때 데몬이 없으면 띄우고, 기다리지 않습니다. |
| `hooks/hook_compact.py` | 압축 직후 「요약은 원문이 아니니 vrs_restore 로 확인하라」 안내 |
| `install.py`, `uninstall.py` | 설치·제거 |
| `docs/VRS_MCP_libvrs_v1.md` | 설계·검증 문서(§14.20까지) |
| `source/` | 태그 v1.0.0 소스 |
| `SHA256SUMS.txt`, `VERSION`, `THIRD_PARTY_NOTICES.md` | 해시, 판, 제3자 고지(mimalloc, MIT) |
## 검증 요약

설계·검증 문서는 `docs/VRS_MCP_libvrs_v1.md` 입니다.

| 항목 | 결과 |
|---|---|
| 엔진 같음 | Win·Linux 동결본과 결과가 같습니다. |
| 원본 정확 | 250항목의 sha·경계가 일치합니다. 260자를 넘는 경로(436자)도 같습니다. |
| 실시간 반영 | 100/100 |
| 이벤트당 비용 | 기록 길이에 따라 자라지 않습니다(비 1.078). |
| 재시작 | 고정 시험에서 1.2–1.5 s |
| 직렬 구조 | 정적 검사 0 |
| 준비 전 요청 | 곧바로 `NOT_READY` 를 돌려줍니다(최대 339 ms). |
| 압축 뒤 복원(분기 시험) | 같은 압축 요약에서 갈랐을 때, 요약이 잃은 18개 사실 가운데 도구 없는 쪽은 0개를 맞혔습니다. 도구 있는 쪽은 16·17개를 맞혀, 미리 정한 기준(17개)에는 한 쌍이 못 미쳤습니다. 놓친 것은 색인 준비 전 질문뿐이고, 색인이 준비된 뒤에는 전부 맞혔습니다. |

## 알려진 문제

- **첫 색인 창 (KM6d)**: 빈 데이터 폴더로 처음 띄우면 `~/.claude/projects` 전체를 색인하는 동안(이 PC 기준 약 25 s) `NOT_READY` 가 나옵니다. 이 동안 모델이 다시 부르지 않고 모른다고 답할 수 있습니다. 한 번 색인한 뒤의 재시작은 1–2 s 입니다.
- **교체 직후 느린 첫 응답 (KL2)**: 라이브 교체 직후 첫 응답이 5–7 s 걸린 적이 두 번 있습니다. 재현되지 않았고 원인은 아직 모릅니다.
- 세션을 넘는 회수와 질의 회수는 이 판에 없습니다.

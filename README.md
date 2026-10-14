# vrs-lib 1.1.0

Claude Code(Windows)용 플러그인입니다. 컨텍스트 압축은 그대로 두고, 압축 뒤에 **압축 전 원문**(사용자 발화와 답)을 바이트 그대로 돌려주는 데만 씁니다.

- 엔진: libvrs lib7(동결본)을 Windows 네이티브로 옮긴 것입니다.
- 실행 파일 하나(`bin/vrs_ccd.exe`)가 데몬, MCP 서버, 훅을 모두 맡습니다. **파이썬도 설치기도 필요 없습니다.**
- `source/` 는 개발 저장소 `plugin` 브랜치(커밋 `cdca61c`)의 소스 전체이고, `bin/vrs_ccd.exe` 는 이 소스로 빌드한 것입니다.

## 설치

Claude Code 에서:

```
/plugin install vrs-lib --marketplace raspie10032/vrs-lib
```

또는 터미널에서:

```bash
claude plugin install vrs-lib --marketplace raspie10032/vrs-lib
```

- 비공개 저장소라서, 이 PC 의 git 이 GitHub 에 로그인돼 있어야 합니다(`gh auth login` 뒤 `gh auth setup-git`, 또는 Git Credential Manager).
- 설치 뒤 **새로 연 세션부터** 적용됩니다.
- 데스크톱 앱에서는 **+ → Plugins** 에서 관리합니다.
- 업데이트는 `/plugin` 메뉴에서, 제거는 `claude plugin uninstall vrs-lib` 로 합니다. 제거하면 플러그인 데이터 폴더(색인)도 지워집니다. 남기려면 `--keep-data` 를 붙입니다.

## 도구

플러그인 도구 이름은 `mcp__plugin_vrs-lib_vrs-lib__vrs_restore` 와 `mcp__plugin_vrs-lib_vrs-lib__vrs_read` 입니다.

| 도구 | 인자 | 하는 일 |
|---|---|---|
| `vrs_restore` | 없음 (`start`, `budget` 은 선택) | 이 세션의 마지막 압축 전 원문 발화·답을 돌려줍니다. 여러 쪽이면 결과의 `next` 를 `start` 로 넘겨 이어 받습니다. |
| `vrs_read` | `path`, `start`, `end` | 대화 기록 폴더와 `VRS_CC_READ_ROOTS`(`;` 로 구분)에 준 폴더 아래 파일의 정확한 바이트 범위를 SHA-256 과 함께 읽습니다. |

`NOT_READY` 는 색인을 준비하는 중이라는 뜻이지 원문이 없다는 뜻이 아닙니다. 몇 초 뒤 같은 인자로 다시 부르면 됩니다.

## 플러그인이 하는 일

- **MCP 서버** (`.mcp.json`): `vrs_ccd.exe --mcp`. 초기화 때 데몬이 없으면 띄우고 기다리지 않습니다.
- **훅** (`hooks/hooks.json`):
  - 세션 시작과 프롬프트 입력 때 `--hook writer`: 열린 세션의 쓰기 표지를 남깁니다.
  - 압축 직후 `--hook compact`: 「요약은 원문이 아니니 vrs_restore 로 확인하라」는 안내를 넣습니다.
- **데이터 폴더** 는 플러그인 데이터 폴더(`~/.claude/plugins/data/…`)입니다. `VRS_CC_DATA` 환경 변수로 바꿀 수 있습니다.

## 저장소 구성

| 경로 | 내용 |
|---|---|
| `.claude-plugin/plugin.json`, `.claude-plugin/marketplace.json` | 플러그인과 마켓플레이스(이 저장소 하나) |
| `.mcp.json`, `hooks/hooks.json` | MCP 서버와 훅 선언 |
| `bin/vrs_ccd.exe` | 데몬·MCP 서버·훅이 든 단일 실행 파일 |
| `docs/VRS_MCP_libvrs_v1.md` | 설계·검증 문서 |
| `source/` | 빌드한 소스 |
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
- **다른 데몬이 먼저 떠 있으면**: 파이프 이름이 사용자마다 하나라서, 예전 수동 설치(1.0.0)의 데몬이 떠 있으면 플러그인이 그 데몬에 붙습니다. 1.0.0 을 쓰던 PC 는 예전 MCP·훅을 지우고 `Get-Process vrs_ccd | Stop-Process` 로 데몬을 끝낸 뒤 새 세션을 엽니다.
- **업데이트 직후**: 떠 있는 데몬은 이전 판 폴더의 실행 파일로 계속 돕니다. 새 판 데몬으로 바꾸려면 위와 같이 데몬을 끝냅니다.
- 세션을 넘는 회수와 질의 회수는 이 판에 없습니다.

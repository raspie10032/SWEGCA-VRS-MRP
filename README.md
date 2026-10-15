# vrs-lib 1.2.0 (SWEGCA VRS-MRP)

**VRS-MRP** 는 Verification-Reinforced Synapse(검증강화 시냅스), Memory Recovery Plugin(메모리 복구 플러그인)이라는 뜻입니다.

Windows 용 플러그인이고, **Claude Code** 와 **ChatGPT 데스크톱 앱**에서 씁니다. 컨텍스트 압축은 그대로 둡니다. 압축된 뒤에 **압축 전 원문**(사용자 발화와 답)을 바이트 그대로 돌려줍니다.

- 엔진은 libvrs lib7(동결본)을 Windows 네이티브로 옮긴 것입니다.
- 실행 파일 하나(`bin/vrs_ccd.exe`)가 데몬, 두 앱의 MCP 서버, 훅을 모두 맡습니다. **파이썬도 설치기도 필요 없습니다.**
- **한 PC 한 경험(1.2.0):** 두 앱이 PC 하나에 데몬 하나와 저장소 하나를 같이 씁니다. Claude Code 기록과 앱 기록이 한 저장소에 겹치지 않게 들어갑니다.
- `source/` 는 `bin/vrs_ccd.exe` 를 빌드한 소스입니다.

## 설치

### Claude Code

Claude Code 에서:

```
/plugin install vrs-lib --marketplace raspie10032/SWEGCA-VRS-MRP
```

또는 터미널에서:

```bash
claude plugin install vrs-lib --marketplace raspie10032/SWEGCA-VRS-MRP
```

- 공개 저장소라 GitHub 로그인 없이 설치됩니다. Git 만 있으면 됩니다.
- 설치 뒤 **새로 연 세션부터** 적용됩니다.
- 데스크톱 앱에서는 **+ → Plugins** 에서 관리합니다.

### ChatGPT 데스크톱 앱

앱에 들어 있는 codex 로 마켓플레이스 `raspie10032/SWEGCA-VRS-MRP` 를 더한 뒤 `vrs-lib` 를 설치합니다.

```bash
codex plugin marketplace add raspie10032/SWEGCA-VRS-MRP
```

```bash
codex plugin add vrs-lib@vrs-lib
```

- 압축 직후 안내 훅은 앱에서 **신뢰**해야 동작합니다. 판이 바뀌어 훅 글이 달라지면 다시 신뢰해야 합니다.
- 앱을 다시 켜면 적용됩니다.

## 도구

| 도구 | 인자 | 하는 일 |
|---|---|---|
| `vrs_restore` | 없음 (`start`, `budget` 은 선택) | 이 세션(앱에서는 이 스레드)의 마지막 압축 전 원문 발화·답을 돌려줍니다. 여러 쪽이면 결과의 `next` 를 `start` 로 넘겨 이어 받습니다. |
| `vrs_read` | `path`, `start`, `end` | 대화 기록 파일의 정확한 바이트 범위를 SHA-256 과 함께 읽습니다. 읽을 수 있는 곳은 Claude Code 기록 폴더, 앱의 `sessions` 폴더, 그리고 `VRS_CC_READ_ROOTS`·`VRS_CX_READ_ROOTS`(`;` 로 구분)에 준 폴더입니다. |

- Claude Code 에서 도구 이름은 `mcp__plugin_vrs-lib_vrs-lib__vrs_restore` 와 `mcp__plugin_vrs-lib_vrs-lib__vrs_read` 입니다.
- `NOT_READY` 는 색인을 준비하는 중이라는 뜻입니다. 원문이 없다는 뜻이 아닙니다. 몇 초 뒤 같은 인자로 다시 부르면 됩니다.

## 플러그인이 하는 일

- **MCP 서버:** Claude Code 는 `.mcp.json`, 앱은 `codex/mcp.json` 을 씁니다. `vrs_ccd.exe --mcp` 가 뜨고, 데몬이 없으면 띄웁니다.
- **훅:**
  - Claude Code(`hooks/hooks.json`):
    - 세션 시작과 프롬프트 입력 때 `--hook writer` 로 열린 세션의 쓰기 표지를 남깁니다.
    - 압축 직후 `--hook compact` 로 「요약은 원문이 아니니 vrs_restore 로 확인하라」는 안내를 넣습니다.
  - 앱(`codex/hooks.json`): 압축 직후 같은 안내를 넣습니다.
- **데몬과 데이터:**
  - 데몬은 PC 사용자마다 하나이고, 파이프는 `\\.\pipe\vrs-lib-<사용자>` 입니다.
  - 데이터 폴더는 `%LOCALAPPDATA%\vrs-lib` 입니다. 어느 앱에도 속하지 않아서, 플러그인을 지워도 남습니다. 지우려면 데몬을 끝낸 뒤 직접 지웁니다.
  - 환경 변수 `VRS_HOME`(데이터 폴더)과 `VRS_ENDPOINT`(파이프)로 바꿀 수 있습니다.
  - 데몬은 원본 기록(`~/.claude/projects`, `~/.codex/sessions`)을 읽기만 합니다.

## 1.1.x 에서 올릴 때

- 1.2.0 데몬은 처음 뜰 때 예전 데이터 폴더를 `%LOCALAPPDATA%\vrs-lib` 로 **이름만 바꿔서** 옮깁니다. 복사는 하지 않습니다.
  - Claude Code 쪽 예전 폴더는 `~/.claude/plugins/data/vrs-lib-vrs-lib` 이고, 앱 쪽은 `~/.codex/vrs-lib` 입니다.
  - 옮긴 목록은 `migrated.txt` 에 남습니다.
  - 이름 바꾸기에 실패한 몫(다른 드라이브, 잠김)은 새로 색인합니다.
- 떠 있는 예전 데몬은 예전 판 실행 파일로 계속 돕니다. `Get-Process vrs_ccd | Stop-Process` 로 끝낸 뒤 새 세션을 엽니다. 그러면 1.2.0 데몬이 뜹니다.
- 이미 열려 있던 Claude Code 세션은 다시 열어야 1.2.0 훅을 씁니다. 앱은 다시 켜야 합니다.

## 저장소 구성

| 경로 | 내용 |
|---|---|
| `.claude-plugin/plugin.json`, `.claude-plugin/marketplace.json` | Claude Code 플러그인과 마켓플레이스(두 앱이 같이 읽음) |
| `.codex-plugin/plugin.json`, `codex/mcp.json`, `codex/hooks.json` | ChatGPT 데스크톱 앱 플러그인 |
| `.mcp.json`, `hooks/hooks.json` | Claude Code 의 MCP 서버와 훅 |
| `bin/vrs_ccd.exe` | 데몬·MCP 서버·훅이 든 단일 실행 파일 |
| `docs/VRS_MCP_libvrs_v1.md` | 설계·검증 문서 |
| `source/` | 빌드한 소스 |
| `SHA256SUMS.txt`, `VERSION`, `THIRD_PARTY_NOTICES.md` | 해시, 판, 제3자 고지(mimalloc, MIT) |

## 검증 요약

### 1.1.x (Claude Code)

| 항목 | 결과 |
|---|---|
| 엔진 같음 | Win·Linux 동결본과 결과가 같습니다. |
| 원본 정확 | 250항목의 sha·경계가 일치합니다. 260자를 넘는 경로(436자)도 같습니다. |
| 실시간 반영 | 100/100 |
| 이벤트당 비용 | 기록 길이에 따라 자라지 않습니다(비 1.078). |
| 재시작 | 고정 시험에서 1.2–1.5 s |
| 직렬 구조 | 정적 검사 0 |
| 준비 전 요청 | 곧바로 `NOT_READY` 를 돌려줍니다(최대 339 ms). |
| 압축 뒤 복원(분기 시험 KM6c′) | 같은 압축 요약에서 갈랐습니다. 요약이 잃은 18개 사실 가운데 도구 없는 쪽은 두 쌍 모두 0개, 도구 있는 쪽은 두 쌍 모두 18개를 맞혔습니다(기준 17개, 오답 0, 도구 호출 1번). |

### 1.2.0 (한 PC 한 경험, 실제 Claude Code 와 실제 ChatGPT 데스크톱 앱)

| 항목 | 결과 |
|---|---|
| 데몬 하나 | 두 앱의 호스트가 각각 `vrs_restore` 를 불러 정상 결과를 받았고, 두 호출 모두 데몬은 1개였습니다. |
| 앱 압축 뒤 복원 | 실제 앱 스레드에서 결과가 가리킨 경계가 실제 압축 위치와 같았습니다. 압축이 있는 앱 스레드 4개의 22항목은 sha 와 글이 모두 같았습니다. |
| Claude Code 회귀 없음 | 압축이 있는 기록 62개의 4850항목 sha 가 같았습니다. 루트가 하나일 때 결과는 1.1.1 과 같습니다(증명과 시험). |
| 재시작·지연 | 재시작 뒤 첫 응답 3.6 s. 앱 기록이 더해져도 지연 비는 0.98–1.02 였습니다. |
| 옮겨 가기 | 예전 데이터 사본 5716개 파일을 이름만 바꿔 옮겼습니다(파일 id 그대로). 되돌리기가 정확했고, 복원 답은 옮기기 전과 바이트까지 같았습니다. |

## 알려진 문제

- **첫 색인 창 (KM6d):** 빈 데이터 폴더로 처음 띄우면 기록 전체를 색인하는 동안 `NOT_READY` 가 나옵니다(이 PC 기준 약 16–25 s). 이 동안 모델이 다시 부르지 않고 모른다고 답할 수 있습니다. 한 번 색인한 뒤의 재시작은 1–4 s 입니다. 엔진(lib7)이 모든 세션에서 센 칸을 읽고 고르기 때문에, 빈 데이터의 첫 답은 원본 전체를 한 번 읽는 시간보다 빨라질 수 없습니다(설계 문서 §14.22 정리 T-d1). 이 하한은 엔진 정의를 바꿔야 없앨 수 있습니다.
- **교체 직후 느린 첫 응답 (KL2):** 라이브 교체 직후 첫 응답이 5–7 s 걸린 적이 두 번 있습니다. 재현되지 않았고 원인은 아직 모릅니다.
- **앱의 압축 뒤 회수 이득:** 이 앱의 압축은 사용자 메시지를 원문 그대로 남기고, 잃는 것은 답과 도구 출력입니다. 그 경우의 회수 이득을 재는 시험(X7′)은 아직 판정하지 않았습니다.
- **예전 데몬:** 1.1.x 데몬이 파이프를 쥐고 있으면 앱 쪽 도구가 `OLD_DAEMON_ON_PIPE` 를 냅니다. 위 「1.1.x 에서 올릴 때」처럼 데몬을 끝냅니다.
- 세션을 넘는 회수와 질의 회수는 이 판에 없습니다.
- Windows x64 만 지원합니다.

## 라이선스

**MIT AND MPL-2.0** 입니다. MIT(`LICENSE`)가 기본이고, `source/native/libvrs/swegca_cpp/` 만 SWEGCA-Architecture 에서 옮긴 코드라 MPL-2.0(`LICENSES/MPL-2.0.txt`)입니다. 경계는 `NOTICE.md` 에, 제3자 고지(mimalloc, MIT)는 `THIRD_PARTY_NOTICES.md` 에 있습니다.

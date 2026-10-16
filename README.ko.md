# vrs-lib 1.3.4 (SWEGCA VRS-MRP)

[English](README.md) | **한국어**

**VRS-MRP** 는 Verification-Reinforced Synapse(검증강화 시냅스), Memory Recovery Plugin(메모리 복구 플러그인)이라는 뜻입니다.

Windows 용 플러그인이고, **Claude Code** 와 **ChatGPT 데스크톱 앱**에서 씁니다. 컨텍스트 압축은 그대로 둡니다. 압축된 뒤에 **압축 전 원문**(사용자 발화와 답)을 바이트 그대로 돌려줍니다.

- 엔진은 libvrs lib7(동결본)을 Windows 네이티브로 옮긴 것입니다.
- 실행 파일 하나(`bin/vrs_ccd.exe`)가 데몬, 두 앱의 MCP 서버, 훅을 모두 맡습니다. **파이썬도 설치기도 필요 없습니다.**
- **저절로 떠오르는 하나의 경험(1.3.1):** 보내는 말이 곧 단서입니다. 모델이 답하기 전에, 그 말이 두 앱의 저장소 하나에서 떠올린 기억(다른 Claude Code 세션과 ChatGPT 앱 스레드)을 원문 그대로, 기억마다 어느 앱의 어느 세션인지와 함께 그 턴에 넣습니다. 도구를 부르지 않아도 되고, 지금 세션의 턴은 넣지 않습니다. 기억은 강도 → 맞은 낱말 수 → 최근 순으로 옵니다. 강도는 보낸 말의 드문 낱말(이름·식별자·숫자)에서 나온 것만 셉니다. 그래서 어디에나 있는 낱말 하나가 기억 하나를 맨 앞으로 끌어오지 않습니다(1.3.4). ChatGPT 앱에서는 새 스레드의 첫 말부터, 앱이 훅을 PowerShell 로 돌릴 때도 동작합니다(1.3.3).
- **두 앱을 가로지르는 검색(1.3.0):** `vrs_search` 는 어디서 묻든 이 PC 의 모든 Claude Code 세션과 앱 스레드에서 질문에 답하는 턴을 찾습니다. 가장 강하게 기억된 턴은 원래 위치와 함께 원문 그대로 돌아옵니다.
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
| `vrs_search` | `query` (`start`, `budget` 은 선택) | 이 PC 의 모든 Claude Code 세션과 앱 스레드에서 찾습니다. 앞 행은 가장 강하게 기억된 턴을 원문 그대로 다시 보여 줍니다(순서: 질문 낱말과 그 기억의 시냅스 강도 → 맞은 낱말 수 → 최근). 나머지 행은 맞는 턴을 모두 위치로 알려 주고, `vrs_read` 로 읽을 수 있습니다. 드문 낱말(이름·식별자·숫자)은 오래된 턴까지 닿고, 64개 넘는 턴에 나오는 낱말은 최근 64개로 봅니다. 여러 쪽이면 `next` 를 `start` 로 넘깁니다. |

- Claude Code 에서 도구 이름은 `mcp__plugin_vrs-lib_vrs-lib__vrs_restore`, `mcp__plugin_vrs-lib_vrs-lib__vrs_read`, `mcp__plugin_vrs-lib_vrs-lib__vrs_search` 입니다.
- `NOT_READY` 는 색인을 준비하는 중이라는 뜻입니다. 원문이 없다는 뜻이 아닙니다. 몇 초 뒤 같은 인자로 다시 부르면 됩니다.
  - 설치 직후 처음 뜰 때는 기록 전체를 색인하느라 이 상태가 수십 초 이어질 수 있습니다. 그 뒤의 재시작은 몇 초입니다.

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

## 1.3.0–1.3.3 에서 올릴 때

- `Get-Process vrs_ccd | Stop-Process` 로 데몬을 끝내고 새 세션을 엽니다(ChatGPT 앱도 다시 켭니다). 1.3.4 데몬이 같은 데이터로 뜹니다.
- ChatGPT 앱에서는 새 UserPromptSubmit 훅을 한 번 신뢰해야 합니다(앱이 묻거나, 훅 목록에서 확인).

## 1.2.0 에서 올릴 때

- 떠 있는 데몬은 검색이 없는 1.2.0 실행 파일로 계속 돕니다. `Get-Process vrs_ccd | Stop-Process` 로 끝낸 뒤 새 세션을 엽니다. 그러면 1.3.0 데몬이 뜨고 검색 색인을 짓습니다(기록이 많으면 처음 한 번은 1분쯤 걸릴 수 있습니다).

## 1.1.x 에서 올릴 때

- 1.2.0 데몬은 처음 뜰 때 예전 데이터 폴더를 `%LOCALAPPDATA%\vrs-lib` 로 **이름만 바꿔서** 옮깁니다. 복사는 하지 않습니다.
  - Claude Code 쪽 예전 폴더는 `~/.claude/plugins/data/vrs-lib-vrs-lib` 이고, 앱 쪽은 `~/.codex/vrs-lib` 입니다.
  - 옮긴 목록은 `migrated.txt` 에 남습니다.
  - 이름 바꾸기에 실패한 몫(다른 드라이브, 잠김)은 새로 색인합니다.
- 떠 있는 예전 데몬은 예전 판 실행 파일로 계속 돕니다(이때 앱 쪽 도구는 `OLD_DAEMON_ON_PIPE` 를 냅니다). `Get-Process vrs_ccd | Stop-Process` 로 끝낸 뒤 새 세션을 엽니다. 그러면 1.2.0 데몬이 뜹니다.
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
| `README.md`, `README.ko.md` | 이 안내(영어, 한국어) |
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

### 1.3.4 (회상 순서, 실제 Claude Code·앱 기록의 고정 사본)

| 항목 | 결과 |
|---|---|
| 적중 | 새 스레드에서 물은 실제 질문 30개 중 16개에서 떠오른 기억에 답이 든 기억이 있었습니다(1.3.3 은 13개). |
| 순서 | 30/30: 떠오른 기억이 가장 높은 강도를 같이 갖고, 맞은 낱말 수 → 최근 순이었습니다. 흔한 낱말에서 나온 강도는 없었습니다. |
| 정확한 원문과 출처 | 30/30: 떠오른 기억 133건, 항목 281개의 sha 와 글이 원본과 같았고, 앱·세션이 맞았습니다. |
| 크기와 속도 | 말 하나에 최대 9,909 UTF-16 단위, 회상이 더하는 시간 중앙값 5.46 ms(p95 7.55 ms). |
| 회귀 없음 | `vrs_search`(30/30)·`vrs_restore`·`vrs_read`(34/34) 답이 1.3.3 과 같고, K5 덤프가 독립 계산기와 같았습니다(14,270줄). |

### 1.3.3 (말마다 회상, 실제 Claude Code·앱 기록의 고정 사본, 그다음 실제 두 앱)

| 항목 | 결과 |
|---|---|
| 정확한 원문과 출처 | 실제 질문 30개 모두: 떠오른 기억 129건, 항목 256개의 sha 와 글이 원본과 같았고, 앱·세션이 맞았습니다. |
| 크기 | 말 하나에 최대 9,554 UTF-16 단위(Claude Code 상한 10,000). |
| 속도 | 회상이 더하는 시간 중앙값 6.20 ms(p95 9.71 ms). |
| 새 스레드 | 기록이 아직 없는 스레드도 회상합니다(30/30). |
| 회귀 없음 | `vrs_search`(30/30)·`vrs_restore`·`vrs_read`(34/34) 답이 1.3.0 과 같고, K5 덤프가 독립 계산기와 같았습니다(14,270줄). |
| 실제 두 앱 | ChatGPT 앱 새 스레드에서 Claude Code 세션에만 있던 일을 물었더니, 떠오른 기억으로 도구 호출 없이 맞게 답했습니다. Claude Code(`claude -p`, 도구 끔)도 앱 스레드에만 있던 일을 같은 방식으로 맞게 답했습니다. |

### 1.3.0 (두 앱을 가로지르는 검색, 실제 Claude Code·앱 기록의 고정 사본)

| 항목 | 결과 |
|---|---|
| 독립 계산기와 같은 값 | 데몬의 전량 덤프(턴·칸·시냅스·질의)가 따로 짠 계산기와 14,270줄 모두 같았습니다. 경쟁 주입 일꾼 1·12, 짓는 도중 강제 종료 10회 뒤에도 같았습니다. |
| 물은 것을 찾음 | 실제 Claude Code 질문 20개 중 19개, 앱을 가로지르는 질문 10개 중 10개가 첫 쪽에 답을 냈습니다. |
| 정확한 원문 | 다시 보여 준 557항목의 sha 와 글이 원본과 같았습니다. |
| 실시간 | 방금 쓴 줄 100/100 을 찾았습니다(중앙값 10 ms). |
| 재시작 | 재시작 뒤 첫 검색 3.5–4.1 s(16,212 턴). |
| 회귀 없음 | `vrs_restore`·`vrs_read` 답이 1.2.0 과 바이트까지 같았습니다(34/34). |

### 1.2.0 (한 PC 한 경험, 실제 Claude Code 와 실제 ChatGPT 데스크톱 앱)

| 항목 | 결과 |
|---|---|
| 데몬 하나 | 두 앱의 호스트가 각각 `vrs_restore` 를 불러 정상 결과를 받았고, 두 호출 모두 데몬은 1개였습니다. |
| 앱 압축 뒤 복원 | 실제 앱 스레드에서 결과가 가리킨 경계가 실제 압축 위치와 같았습니다. 압축이 있는 앱 스레드 4개의 22항목은 sha 와 글이 모두 같았습니다. |
| Claude Code 회귀 없음 | 압축이 있는 기록 62개의 4850항목 sha 가 같았습니다. 루트가 하나일 때 결과는 1.1.1 과 같습니다(증명과 시험). |
| 재시작·지연 | 재시작 뒤 첫 응답 3.6 s. 앱 기록이 더해져도 지연 비는 0.98–1.02 였습니다. |
| 옮겨 가기 | 예전 데이터 사본 5716개 파일을 이름만 바꿔 옮겼습니다(파일 id 그대로). 되돌리기가 정확했고, 복원 답은 옮기기 전과 바이트까지 같았습니다. |

## 라이선스

**MIT AND MPL-2.0** 입니다. MIT(`LICENSE`)가 기본이고, `source/native/libvrs/swegca_cpp/` 만 SWEGCA-Architecture 에서 옮긴 코드라 MPL-2.0(`LICENSES/MPL-2.0.txt`)입니다. 경계는 `NOTICE.md` 에, 제3자 고지(mimalloc, MIT)는 `THIRD_PARTY_NOTICES.md` 에 있습니다.

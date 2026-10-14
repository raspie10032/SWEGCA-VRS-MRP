# N-G2 작성자 표지 집합: 정의 → 수식 → 증명 → 알고리즘 (구현 전)

2026-10-13. 출발 ng2=0d40fe5. RULES/ROUND2, §13·§14.0–14.10,
G의 NATIVE_G.md G-T1을 읽었다. lib7/L1 판단과 N1–N5 문턱은 그대로다.

## 1. 정의 및 먼저 고정한 끝 조건

F는 원본 집합, N=|F|, s(f)는 append-only 원본 크기, c(f)는 기존 cover.
M은 경로 SHA256 이름의 불변 파일 집합이다. 내용은 정규화된 원본 경로다.
각 append 완료 뒤 작성자가 publish(M,f)를 성공시킨다(P 계약). 집합 합류는
멱등이며 전역 순서·로그·커서·주인이 없다. 다른 경로의 해시 충돌은 오류로
검출하며 합치지 않는다. P가 확인되지 않은 원본에 표지 전용 복구를 적용해
N5 통과라고 하지 않는다. 초기 전체 스캔은 기존 시작 계약이며 사건 복구와
구분한다. 부모 mtime을 만지는 안은 P개 폴더의 비교가 필요하므로 제외한다.

끝 조건은 사용자 원문 그대로: kwov 실제 overflow와 표지 복구, 동일 Δ에서
N 두 조건의 방문수·시간, static/n1_m1/n1_kx2/w5/w6_500/samples/kx4,
Linux 빌드 및 동결본 JSON(끝 us만 제외)/rc/hash 같음. 기준을 낮추지 않는다.

## 2. 수식

T_publish = O(경로 길이), T_recover = O(|M| + 표지 경로 길이 합 + BΔ +
기존 work/lib 비용). 정적 F 열거 항은 0이다. 정상 처리도 cover가 EOF를
덮으면 표지를 지우므로 M은 미처리/진행 중 변경 파일의 집합이지 전체 이력이
아니다. 불완전 줄/변환 오류의 표지는 보존한다(누락을 청소로 숨기지 않는다).
이는 |M|=|Δ|라는 무조건 주장이나 기존 lib7 늦은 턴 비용 제거 주장이 아니다.

## 3. 정리

G2-T1(완성 게시). 작업용 임시 파일에 경로 전체를 쓰고 닫은 뒤 최종 해시
이름에 hard-link로 게시한다. 기존 이름은 같은 경로인지 확인한다. 독자는
완성된 불변 내용만 읽는다. 임시 파일은 M 밖에 두어 M 열거에 끼지 않는다.
생성 중 죽은 임시 파일은 표지가 아니며 append와 게시 사이 사망은 P 실패다.

G2-T2(삭제/재게시 경합). 청소자는 c를 읽고 s≤c를 확인한 뒤 m을 지운다.
그 뒤 반드시 원본 크기 s2를 다시 읽는다. s2>c면 m을 재게시하고 work를
표시한다. append a와 삭제 d, 뒤 크기 확인 r에 대해:
(a≤d) a의 표지가 d로 지워져도 r은 늘어난 크기를 본다.
(d<a≤r) 역시 r로 복구하거나 작성자 게시가 남는다.
(r<a) a 뒤 작성자 게시가 남는다. 여러 청소자도 각 삭제 뒤 같은 의무를
가지므로 동일하다. 변환 cover는 줄지 않는다. 죽은 데몬은 기존 시작 전체
스캔으로 복구한다. 전원 상실의 디스크 내구성까지 이 정리로 주장하지 않는다.

G2-T3(빠짐없음, P 조건부). 넘침 전에 완료된 미처리 append는 M에 있거나
G2-T2의 실행 중 청소자가 재표시할 책임을 가진다. 감시 재등록 후 M을 열거해
기존 work를 표시한다. 열거 중/뒤 새 게시에는 감시 통지 또는 다음 overflow가
있다. 유한 변경이 멎고 OS/worker가 진행하면 완전한 줄은 모두 cover에 든다.
성공 삭제는 cover를 바꾸지 않으므로 C(T)/lib7 판단은 동일하다.

G2-T4(훅만으로 P를 증명할 수 없음). f 크기100에서 훅→m→work→c=100→삭제,
그 뒤 비동기 기록으로 f=200, 다음 훅 없음, 해당 통지 손실인 실행에서 M은
빈 집합이다. 표지 열거는 f를 찾지 못한다. 마지막 훅 뒤 임의 sleep/유한
재확인은 같은 반례의 시각을 늦출 뿐이다. 전체 세션 표지를 영구 유지하면
|M|=N이 되어 비용 조건을 어긴다. 따라서 표지 자리는 실제 transcript writer의
append 완료/flush 지점이어야 한다. 작성자가 죽기 전 반드시 게시할 계약이
없으면 두 파일 사이 원자성도 별도 증명 의무다. L1 판단 변경이 아니라 입력
관측 경계 L1 결함, G 막음이다. 실호스트 교체 권한/소스 없는 상태에서 이를
충족했다고 선언하지 않는다. 범위 안의 구현·검증은 계속한다.

## 4. 알고리즘 (코드 전에 고정)

- 네이티브 데몬 실행 파일에 독립 --mark-root M --mark-path f 및 --mark-hook M
  단발 명령을 둔다. 훅 명령은 stdin JSON에서 transcript_path만 사용한다.
  기존 SHA256/UTF8 처리 사용, transcript 전체 파싱/읽기 없음.
- --change-markers M은 P 계약 작성자용 명시적 실험 경로다. 기본 외부 작성자
  경로는 기존 N5 전체 복구를 유지한다. 기본 경로의 O(N) 결함은 열린 채 보고한다.
- 표지 경로는 M만 OS 감시한다. 시작 때 F를 한 번 스캔하며 이후 디렉터리
  생성에도 F 하위 재귀 스캔하지 않는다. Linux 새 하위 폴더 감시 필요도 없다.
- overflow: M flat 열거→내용/해시/원본 루트 검사→mark(f). work 완료 및
  cover 단락에서 G2-T2 청소. 뒤 크기 증가에는 기존 무순서 비트셋에 표시한다.
- Windows 기존 ReadDirectoryChangesW/OS 대기, Linux 기존 inotify/poll를
  재사용한다. 새 스레드·잠금·대기 루프·자료 완료자를 통한 직접 다음 단계는 없다.
- kwov 기존 무표지 N5 시험을 보존하고 표지 전용 N=64/4128, Δ=37을 추가한다.
  데몬만 시험 도중 정지해 실제 overflow를 만들고 원본 32+휴면1+신규4 변경,
  표지 게시, 재개한다. cover·원본 sha/위치/본문·중복·후속 변경을 검사한다.
- 훅 입력 네이티브 호출 비용과 실제 Codex 격리 실행의 훅/rollout 시점을
  별도 기록한다. Claude 실행기 없는 Linux에서 Claude 실측이라 하지 않는다.

## 5. 관측 근거

Claude 공식 https://code.claude.com/docs/en/hooks (2026-10-13 확인):
transcript는 비동기 기록이며 Stop 때 마지막 메시지가 아직 없을 수 있다.
OpenAI 공식 https://learn.chatgpt.com/docs/hooks : transcript_path는 nullable,
훅은 수명주기 사건이며 transcript 형식은 안정 인터페이스가 아니다.
설치 Codex CLI=0.153.4. 로컬 소스 버전과 구분해 후속 실측을 기록한다.

실제 ~/.claude/hooks/settings.json/~/.claude.json 및 자격 증명은 읽거나 쓰지
않는다. 모든 제품 변경은 이 작업 트리, 시험 자료도 이 작업 트리 아래다.

## 6. 구현 전 보완: 이미 존재하는 표지도 통지를 낸다

불완전 줄 때문에 m이 남은 경우 다음 append의 publish가 EEXIST만 받고 끝나면
새 감시 사건이 없을 수 있다(L2/G 막음). 기존 m 핸들을 OPEN_EXISTING으로
열고 같은 불변 경로 바이트를 같은 위치에 다시 쓴다. 길이/내용은 동일하므로
독자가 반쪽 내용을 보는 전이는 없고, 쓰기 통지만 다시 생긴다. 그 사이
청소자가 이름을 지웠다면 G2-T2의 삭제 뒤 크기 확인이 책임진다. 이 보완은
집합/멱등 의미와 비용을 바꾸지 않는다.

## 7. 격리 호스트 시험과 경합 시험 (실행 전)

설치된 Codex 0.153.4를 빈 전용 설정 폴더와 localhost SSE 응답기로 돌린다.
실제 서비스/인증/모델 과금 없이 실제 CLI의 훅 발화와 rollout 기록을 본다.
SessionStart/UserPromptSubmit/PreToolUse/PostToolUse/Stop/SubagentStop/PreCompact
및 SessionEnd를 기록하고, 응답 중간 assistant 메시지 뒤와 Stop 시점의 원본
크기를 보존한다. 이는 실제 CLI+모의 모델 시험이며 실제 원격 모델 증거가 아니다.
Linux에 Claude 실행 파일은 없으므로 Claude 지연을 Codex 결과로 대체하지 않는다.

경합 증명은 가능한 append/publish와 cover-check/unlink/recheck 사이 모든
작은 순서 섞기를 열거하는 독립 모델로 검산한다. 실제 데몬에서는 같은 파일에
여러 프로세스가 멱등 표지를 게시하고 별도 writer가 append하는 stress를 더한다.
제품에 배치별 스레드나 시험 동기화 장치를 넣지 않는다.

## 8. Linux 및 호스트 실측 (2026-10-13 18:07 KST)

제품 커밋 6ad2c15. 전체 Linux 빌드 성공(mimalloc2.1.7), CPU0–11.
동결본 비교는 m1 batch/reopen 각409, kx2 각1206: 총3230 JSON 같음,
끝 us만 제외. rc 목록·items/turns/sessions/cells_n도 같음.
cells_hash는 m1=280399ba4c7e1040, kx2=03085bfb04024542.
증거 `docs/evidence/g2/linux-n1.json` (source rev와 바이너리 SHA 포함).

실제 IN_Q_OVERFLOW, 원래 무표지 시험도 통과(36파일·164→166 발화,
restore0). 표지 시험 Δ37 고정 결과:

| 초기 F | 복구 표지 열거 | 열거 µs | 훅 명령 프로세스 중앙값 µs |
|---:|---:|---:|---:|
|64|37|660|620.396|
|4128|37|748|632.936|

후속 동시게시 시험도 각각64회 오류0, 최종136 발화 원본/sha/위치 같음,
남은 표지0. 그 재실행의 열거는37/37 및638/538µs. 결과의 recovered_us는
Python 검증기가 F 전체 cover를 확인하는 시간이 포함되므로 제품 복구 비용
비교에 쓰지 않는다. 복구 시간 근거는 데몬 내부 VRS_TEST_MARKERS us다.

독립 모델은 청소자1의15가지, 청소자2의3150가지 순서 섞기 전부에서
`s≤c 또는 m 존재`를 확인했다. 이것은 G2-T2 검산이며 호스트 훅 증명은 아니다.

실제 설치 Codex0.153.4 + localhost 모의 모델 + 격리 설정 시험(rc0):
SessionStart/UserPromptSubmit 시31874B → 훅 없는 중간 assistant 뒤33519B →
Stop 시35713B → SessionEnd/종료 시36016B(task_complete 추가).
따라서 쓰는 쪽 수명주기 훅은 **있다**. 매 append 훅은 아니다. 도구/서브에이전트/
압축 없는 이 실행에서 PreToolUse/PostToolUse/SubagentStop/PreCompact가
발화하지 않은 것은 정상이며, 그 사건들의 모든 실행을 검증한 것은 아니다.
UserPromptSubmit 이후 최소1645B가 다음 훅 전에 저장된 실측이다.
Caller 세션의 명시 입력은 기존 브리지/restore 길로 넣지만 다른 세션의 이
구간을 덮지 못한다. SessionEnd도 강제종료/장기 실행 중간을 덮지 못한다.

로컬 Codex 소스3da13917(설치0.153.4와 다른 판)은
`codex-rs/rollout/src/recorder.rs:2091`의 write_line에서 write_all→flush를 한다.
수명주기 hook_runtime이 아닌 이 **실제 쓰기 완료 자리**가 P 인터페이스의
통합 후보이다. 외부 소스/설치 바이너리는 수정하지 않았다. append와 표지
사이 프로세스 사망까지 다루려면 완료 후 훅만으로는 부족하며, 작성자의
사전 지속 의도/완료 프로토콜 또는 OS의 무손실 기록이 추가로 필요하다.
단순 사전 m 생성도 읽는 쪽이 쓰기 전에 청소할 수 있어 반례를 고치지 못한다.
현재 정의가 경로 하나뿐인 m인 이상 그 프로토콜을 구현했다고 주장하지 않는다.

## 9. 사용법과 새 대기 위치

격리 시험에서만 사용한다. `M` 및 `M.tmp`는 같은 파일시스템이어야 한다.

```sh
mkdir -p /isolated/marks
./vrs_ccd --projects /isolated/projects --data /isolated/data \
  --endpoint @vrs-g2 --change-markers /isolated/marks
# 실제 append가 끝난 작성자 코드에서 호출 (성공 여부를 반드시 확인)
./vrs_ccd --mark-root /isolated/marks --mark-path /isolated/projects/session.jsonl
# 훅의 JSON stdin을 받는 편의 인터페이스. 이것만 설치해서 N5가 되지는 않는다.
./vrs_ccd --mark-hook /isolated/marks
```

Claude의 UserPromptSubmit/PreToolUse/PostToolUse/Stop/SubagentStop/PreCompact/
SessionStart에 넣을 공통 명령은 위 --mark-hook 한 줄이다. Codex도 실측된
transcript_path로 같은 명령을 쓸 수 있다. 실제 설정은 생성/수정하지 않았다.
훅 실행/JSON 입력/원본 존재/게시 실패는 비영 종료이며 조용히 성공하지 않는다.
CLI는 최대1MiB 훅 JSON을 읽고 기존 파서로 처리한다. 비용은 해당 훅 입력 크기와
경로 길이이며 원본 이력은 읽지 않는다. 직접 --mark-path는 JSON 파싱도 없다.

제품에 새 동료 완료 대기는0. 기존 감시 대기 src/vrs_ccd.cpp:299(Windows,
통지/종료), :353(Linux, 통지/종료)를 그대로 사용한다. 새 단발 명령의
:721 fread는 호출자 stdin EOF, cc_markers.hpp:91/96은 자기 파일 쓰기 I/O다.
cc_markers.hpp:79/82은 OS 난수 요청(부팅 초기에는 엔트로피 준비),
cc_io.hpp:124/146은 자기 위치 I/O 완료를 기다리는 기존 자리다.
기존 cc_io.hpp의 위치 I/O 완료 대기를 재사용한다. 작업 집합/스레드 구조는
그대로이며 복구 결과 순서는 의미가 없다. 시험용 pause/poll/ThreadPool은
제품 구조와 구분한다. 임시 게시 파일은 성공/실패 반환 시 제거되며 작성자
강제종료 시 고아 임시 파일이 남을 수 있다. M 열거에는 포함되지 않는다.

## 10. 열린 결함 (끝 조건 유지)

- G2-K1, L1 입력 관측 경계, G 막음: 실제 Claude/Codex 모든 기록의 P 보장
  및 append→표지 사이 작성자 사망 보장이 없다. 수명주기 훅만 설치해 해결했다고
  할 수 없다. 실제 Claude 실행기는 이 Linux에 없어 Claude 발화 시점 실측은
  미실시다. 공식 비동기 기록 계약과 Codex 실측을 서로 대체하지 않는다.
- G2-K2, L2, G 늦춤: P 없는 기본 외부 원본 복구는 여전히 O(N)이다. 기존
  N5를 훼손하지 않기 위해 남겼으며 조건부 표지 시험 성공으로 비용 완료를
  선언하지 않는다. 표지 전용 경로 자체의 N 항은 제거됐다.
- 기본 NOFILE4096에서 큰 fixture는 핸들 용량 부족 가능. 이번4128 시험
  프로세스만 soft32768로 설정했으며 기존 시스템/다른 프로세스는 바꾸지 않았다.

Windows 중계 g2-001(전체 지정 시험)은 진행 중이며 결과를 뒤에 기록한다.

## 11. g2-001 Windows 결함 → L2로 복귀 (수정 코드 전)

6ad2c15의 static/N1/W3/W4/W5(200)/W6(500,bad0)/samples/kx4는 통과,
M8=1.282/1.203/1.202/1.228/1.198s. kwov 표지 경로는
MARKER_REMOVE_FAILED로 실패했다(원래 무표지 시험은 완료). 기준 그대로다.

G2-K3, L2 Windows 이름 삭제 의미, G 막음(표지 경로 N5 인증).
std::filesystem::remove의 DeleteFile 방식은 열린 다른 핸들이 남아 있으면
삭제 보류 상태가 된다. POSIX unlink처럼 이름을 즉시 없앤다는 G2-T2 구현
전제가 성립하지 않는다. Windows 전용 marker_unlink를 DELETE 접근·공유3종으로
열고 SetFileInformationByHandle(FileDispositionInfoEx,
DELETE|POSIX_SEMANTICS)로 이름을 제거한다. 기존 독자 핸들은 계속 읽는다.
임시 파일도 같은 원시로 지워서 hard-link 파일의 다른 이름/핸들에 삭제 보류를
남기지 않는다. 지원 안 되는 파일시스템/OS는 오류 번호를 내며 조용한 fallback,
잠금, 회전 재시도, sleep을 두지 않는다. Linux unlink는 그대로다.

G2-T2의 d를 이 native namespace 삭제로 대응시킨다. 삭제 성공/이미 없음 뒤
크기 재확인은 반드시 그대로 실행한다. 동시 삭제자는 이름 없음만 성공 취급한다.
다른 오류는 코드와 함께 실패로 남긴다. 데이터·판단·work 비용식 변화는0이다.
근거: Microsoft FILE_DISPOSITION_INFORMATION_EX의 POSIX semantics 설명,
FILE_INFO_BY_HANDLE_CLASS의 FileDispositionInfoEx, SetFileInformationByHandle.

## 12. g2-004 진단 → 이름의 이미 진행 중인 삭제를 구분 (코드 전)

02d34f9와 같은 제품의4814f7d에서 전체복구 후 오류는
MARKER_DELETE_OPEN_5 한 건이었다. POSIX 삭제도 SetFileInformationByHandle와
그 DELETE 핸들의 CloseHandle 사이에는 다른 청소자의 CreateFile이
ERROR_ACCESS_DENIED를 받을 수 있다. Win32 error5 전체를 무시하지 않는다.
CreateFile 직후 스레드의 NTSTATUS를 함께 읽어 STATUS_DELETE_PENDING
(0xC0000056)인 경우만, 이미 다른 청소자가 삭제를 개시한 것으로 취급한다.
그 청소자는 취소 없이 핸들을 닫으며 본 호출자도 기존 삭제 뒤 크기 확인을
수행하므로 G2-T2의 d 책임은 사라지지 않는다. 실제 접근거부와 다른 상태는
Win32/NT 코드 둘 다 보존해 실패한다. 새 대기/재시도/규칙 완화 없음.
이 상태가 실측과 다르면 다시 L2/L3 원인을 조사한다.

## 13. g2-005 Windows 표지 경로 통과

7669838494213beecb03ff963158e341262c12a3에서 build/static/kwov 통과.
원래 무표지 N5(36파일,164→166발화)도 유지, 표지 두 조건도 성공했다.

|초기 F|변경Δ|주 복구 열거|열거µs|native 훅 프로세스 중앙값µs|최대µs|
|---:|---:|---:|---:|---:|---:|
|64|37|37|12963|9210.7|13935.6|
|4128|37|37|16105|9842.0|13736.6|

M에서 원본 경로를 읽고 mark하는 native 열거 시간이다. Python cover 검증은
제외한다. 각 조건 실제 overflow87/77회(64B OS 버퍼로 강제), 최종136발화,
64회 동시게시·부분줄·후속변경·cover=EOF·원본 바이트/sha/위치/본문 검증,
누락/중복/변환오류0, 최종표지0, restore0. 정적 원본4064개 증가에 주 복구
방문 증가는0. 위 시간비1.2424만으로 복잡도를 주장하지 않고 코드의 F열거0과
방문37/37을 함께 근거로 둔다. 실제 Claude hook 전체 지연이 아닌 동일
네이티브 --mark-hook 프로세스의 stdin 전달→종료 지연이다(Defender 그대로).

K3는 Windows 파일 삭제 의미를 수정해 닫았다. K1(P 호스트 보장), K2(기본
무표지 O(N))는 이 성공으로 닫히지 않는다. 동일 커밋의 나머지 N1/N3/N2
회귀는 g2-006으로 요청했다. static/kwov를 같은 소스로 불필요하게 반복하지 않는다.

Windows 원시 근거:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/ns-ntddk-_file_disposition_information_ex
- https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ne-minwinbase-file_info_by_handle_class
- https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileinformationbyhandle

RtlGetLastNtStatus는 런타임에서 조회하며 없으면 임의 오류를 성공 처리하지
않는다. Windows FS의 이미 삭제 진행 중인 NTSTATUS만 별도로 가른다.

## 14. 경로 정규화 점검에서 찾은 L3 고침 (코드 전)

--change-markers의 입력이 M/처럼 구분자로 끝나면 정규화 전에 붙인 .tmp가
M/.tmp가 되어 표지 열거 안에 섞인다. L3/G 막음(해당 합법 경로 입력).
M 자체를 만든 뒤 먼저 canonical하고 그 결과로 M.tmp를 만든다. 알고리즘/
표지 정의는 그대로다. kwov의 daemon 옵션에 끝 구분자를 붙여 이 입력도
고정 검증한다. 다른 기준은 바꾸지 않는다. CLI --mark-root는 이미 canonical
뒤에 임시 폴더를 만들어 해당 결함이 없다.

## 15. g2-006 필수 회귀 결과

7669838의 build/n1_m1/n1_kx2/w3/w4/w5/w6_500/samples/kx4 전부 통과.
W5 200회, W6 500회 bad0, 표본66/0/167/17 및 C(T) 같음/원본불일치0.
M8=1.359/1.231/1.216/1.193/1.215s. E-kx1 크기비1.0000003287,
열림뒤60초 증가0, E-kx2 오류0. raw 결과 docs/evidence/g2/g2-006.json.

후속fc76e46은 --change-markers가 비어 있지 않은 시작 가지의 canonical 순서만
고친 제품 차이다. native/libvrs·변환기·lib 도구 소스는7669838과 같고 기본
데몬 실행 길도 같다. 그 표지 시작 길과 최종static은 g2-007로 별도 검증한다.
Linux fc76e46 재빌드/static/끝구분자 N64/4128 시험은 모두 통과,
표지 주 열거37/37 및569/714µs. source-boundaries.json에 소스/바이너리SHA,
출발판 대비 판단 소스 diff0을 기록했다.

## 16. g2-007에서 발견한 읽기/삭제 경합 (코드 전)

fc76e46은 build/static 통과지만 kwov 중 watch: FILE_OPEN_FAILED로 실패했다.
끝 구분자 고침과 별개로 삭제 중인 표지의 **읽기 열기**도 Win32 error5가
될 수 있다. 앞선 g2-005 통과 한 번으로 이 수명 경합을 닫았다고 보지 않는다.
G2-K4, L2 marker handle lifetime, G 막음(N5). 기준 유지.

Windows 표지 전용 open을 분리해 CreateFile 직후 Win32/NTSTATUS를 얻고,
이름 없음 또는 정확한 STATUS_DELETE_PENDING/STATUS_FILE_DELETED만
retired로 돌려준다. 일반 File/원본/cover 열기의 오류 의미는 바꾸지 않는다.
이미 연 핸들은 File에 소유권을 넘기는 Windows 생성자 하나로 기존 위치I/O를
재사용한다. 표지 읽기와 기존 표지의 재통지 쓰기가 이 open을 쓴다.
게시 hard-link도 Windows native 호출 직후 같은 상태를 가른다.

정리 G2-T2 확장: m이 이미 retired면 그 이름을 없애는 청소자는 c≥s를
확인했으며 취소 없이 닫고 삭제 뒤 크기를 재확인한다. 따라서 읽는 쪽은
해당 표지를 생략해도 그 책임이 사라지지 않는다. append 뒤 게시가 retired를
보아도 그 append는 삭제 뒤 크기 확인에 들어간다. 임의 ACCESS_DENIED는
성공으로 간주하지 않는다. 삭제 진행 중 상태와 Win32 오류의 대응이 맞을
때만 인정하고 다른 오류는 두 코드를 함께 보존한다. 대기/재시도0.

## 17. g2-008 native handle lifetime 재검증

20afbcd3bc2a0cd4487617d895c19e44256b0709의 build/static/kwov/samples 통과.
원래N5와 표지N5 모두 통과하며 표지 F64/4128의 주 열거37/37,
15083/14953µs. 각각64동시게시, 최종136발화, 오류/누락/중복0, 잔여표지0.
표본66/0/167/17 및 C(T)/원본 모두 같음. Linux도 새 marker_open 경로로
64/4128 재검증 통과(열거37/37,757/651µs). 한 번 통과 뒤 다른 경합이 나온
이력이 있으므로 동일20afbcd의 전체 필수 회귀와 kwov를 g2-009로 재확인한다.
추가 제품 수정은 없고 K1/K2의 외부 호스트 관측 경계도 유지한다.

## 18. 최종 판정 및 인계

최종 제품20afbcd3bc2a0cd4487617d895c19e44256b0709, g2-009에서
build/static/n1_m1/n1_kx2/w3/w4/w5/w6_500/samples/kx4/kwov 전부 통과.
같은 제품의 g2-008도 static/kwov/samples 통과했다. 코드 추가 변경 없음.
W5 200회, W6 500회 bad0, 표본66/0/167/17와 C(T)/원본 모두 같음.
M8=1.479/1.200/1.184/1.185/1.210s(다섯 번≤5초), E-kx1 크기비
1.0000003287, 열림뒤60초 증가0, E-kx2 오류0.

최종 Windows F64/4128, Δ37 고정: 주 복구 열거37/37,
11581/15460µs. native 훅 프로세스 중앙값9281.7/9405.9µs,
최대11821.9/19203.4µs. 각각64동시게시·부분줄·후속변경·136발화 원본검증,
오류/누락/중복/남은표지0, restore0. 실제overflow85/77회.
최종 Linux의 같은 변경 경로는37/37 및757/651µs, 오류0.

K3/K4의 Windows 수명 경합과 경로 정규화 결함은 고쳤다. K1(실호스트의
매append 표지/작성자사망 보장 없음), K2(기본 외부 경로 O(N))는 그대로
열려 있다. 따라서 **조건부 표지 경로 검증 완료, 전체 G 무조건 완료 아님**.
이 구분과 모든 실패 이력을 REPORT_G2.md 및 relay/notes/h.md에 남긴다.
제품 소스는20afbcd이며 뒤의 증거/문서 커밋은 같은 제품을 설명한다.

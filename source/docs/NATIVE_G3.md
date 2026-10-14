# N-G3 작성자 생존 표지 — 정의 → 수식 → 증명 → 알고리즘

2026-10-13, ng3=ng2 518dbe5. RULES/ROUND2, 보호 문서 §13·§14.0–14.10,
REPORT_G2, NATIVE_G2 T1–T4/K1/K2, relay/notes/g2의 정의 결정을 읽고 작성했다.
**이 문서는 제품 코드 전에 작성한다. L1/lib7 판단·N1–N5 문턱 변경 없음.**

## 1. 정의와 고정한 끝 조건

F=원본 집합, N=|F|, s(f)=append-only 크기, c(f)=완료된 변환 cover.
G2의 M, 완성 멱등 게시, 삭제 뒤 크기 재확인 의무는 그대로다.
W의 원소 w=(f,p,b)는 원본 경로, 작성자 PID, OS 생성 시각 원시 정수다.
파일 이름은 그 불변 내용의 SHA256이다. 같은 f에 재개한 작성자가 겹쳐도
각 (p,b)는 별도 원소라 살아 있는 표지를 죽은 작성자의 청소가 지우지 않는다.
이는 W/m(f)의 여러 작성자 실현이며 순서열·커서가 아니다.

전제 H: SessionStart 및 멱등 보강 UserPromptSubmit 훅이 성공한 작성자이고,
그 작성자의 모든 append는 생존 중에 일어난다. f가 생기기 전 훅이면
가장 가까운 존재하는 부모로 정규화해 f의 이름을 게시한다. 훅 실패는 비영
종료이며 H를 충족했다고 하지 않는다. 훅 없는 원본은 시작 스캔만 보장한다.
H의 실제 설치는 교체 승인 사항이고 이번 작업은 실제 설정을 변경하지 않는다.

A(w)=동일 PID와 생성시각 프로세스가 살아 있음. 프로세스 정보를 못 읽으면
Unknown이고 은퇴하지 않는다. Windows OpenProcess QUERY_LIMITED_INFORMATION,
GetProcessTimes(64-bit FILETIME), GetExitCodeProcess; Linux /proc/pid/stat의
starttime 정수(22번째 필드), Z/X는 종료다. UTC 문자열/반올림 비교 없음.
PID 재사용은 b가 다르면 종료다. Linux 부팅 간 tick 재사용은 보수적 false
alive일 수 있어 boot identity도 표지에 저장한다(Windows도 boot 세대 구분을
지원 가능한 범위에서 명시). 같은 tick 안의 PID 재사용은 OS 식별 정밀도
한계로 늦은 은퇴가 될 뿐 다른 작성자를 빨리 은퇴시키지는 않는다.

D(w)는 f와 Claude의 f에서 .jsonl을 뺀 세션 폴더 아래 jsonl이다.
원본 루트나 날짜/프로젝트 전체는 열거하지 않는다. 별도 PID가 부모보다 오래
쓰는 파일은 그 작성자의 별도 W가 필요하다. Codex 자식 rollout의 훅 발화와
작성자 조상 식별은 설치 CLI+격리 localhost 모델로 실측 후 아래에 고정한다.

끝 조건: 사용자 지정 네 결정적 시험, final68/4132·같은 Δ 방문 상한,
데몬/lib overflow의 전체 열거0, Windows static/n1_m1/n1_kx2/reopen/w3/w4/
w5/w6_500/samples/kx4/kwov, M8 다섯 번≤5s, Linux N1 동결본 일치.

## 2. 수식

R=M∪W. V=|M|+|W|+Σ(w∈W)|D(w)|.
T_recover=O(V+표지/경로 바이트+변경 꼬리+기존 lib7 work 비용).
T_hook=O(조상 깊이+그 조상의 열린 핸들 수+경로/훅 입력 길이).
작성자 식별은 이력 파일을 열거하거나 transcript 전체를 읽지 않는다.
|W|=살아 있는 작성자-세션 쌍+미은퇴 종료 작성자-세션 쌍이다.
은퇴 불가 오류/불완전 줄은 감추지 않는다. 조건 없는 |W|=상수 주장은 안 한다.
정상 주기 검사는 O(|W|+Σ|D(w)|); N 항이 없다. 시작 스캔은 기존 계약이다.

## 3. 손으로 따라가는 정리

G3-T1(수명). w의 게시 후 A(w) 또는 Unknown이면 어떤 실행자도 w를
삭제하지 않는다. 데몬만 !A(w)가 확실하고 모든 g∈D(w)의 c(g)≥s(g)를
확인한 뒤 지운다. 같은 원소의 멱등 게시와 동시 은퇴는 G2의 원자 hard-link와
POSIX namespace 삭제를 쓴다. 다른 (p,b)는 다른 이름이라 ABA가 없다.

G3-T2(빠짐없음). 유한 변경 후 OS/worker/데몬이 진행한다고 하자.
1. W 생성 뒤 append: 작성자가 살아 있으므로 w는 남는다. 통지가 사라져도
   overflow가 W를 열거해 work(f)를 표시하고 [cover,size)의 완전한 줄을 읽는다.
2. 작성자 종료 뒤: 더 쓸 수 없다. 마지막 append가 c 밖이면 w를 남겨 work를
   표시한다. c≥s 뒤 삭제하고 s를 다시 읽는다. 다른 작성자가 같은 f에 썼으면
   새 w 또는 G2의 재게시 M과 work가 이를 덮는다.
3. 생성 전 append: 게시 사건의 work가 그 전의 [cover,size)까지 읽는다.
   게시 사건 자체가 손실되어도 W 열거가 같은 일을 표시한다.
4. G2-T4의 f=200 작성자는 아직 살아 있으므로 그 w가 남아 반례가 성립하지 않는다.
표지 없이 죽어 게시도 못 한 작성자는 H 밖이다. 훅 설치만 했어도 실행 실패를
성공으로 바꾸지 않는다. 전원 상실 내구성/삭제·truncate 입력까지 확장하지 않는다.

G3-T3(딸린 파일). D(w)만 열거해 각 g에 T2를 적용한다. 죽은 작성자 은퇴는
D(w) 전체 cover가 EOF인 조건이므로 부모 f만 처리하고 자식을 잊지 않는다.
삭제 뒤 D(w)를 다시 열거해 크기를 확인한다. 별도 생존 작성자가 있으면 자기
w를 가져야 하며 부모 PID의 사망을 자식의 사망으로 해석하지 않는다.

G3-T4(lib 경로). 변환 완료 C(T)를 명시 insert/written으로 넣는 기존 계약은
같다. lib overflow는 데몬 복구 콜백으로 work를 표시한다. work의 cover 단락도
vrs_refresh(C(T))로 해당 파일 하나의 디스크 꼬리를 다시 표시한다. 따라서
변환이 이미 끝났으나 lib 통지가 손실된 경우도 복구된다. standalone lib는
자기 M/W만 열거하고 경로별 ingest를 표시한다. 어떤 overflow에도 sdir 전체
scan_new_dir를 부르지 않는다. 입력/판단 함수는 바꾸지 않는다.

G3-T5(은퇴 진행). W 생성/변경 사건, 넘침, 평상시 단조시계 주기마다 W를
순서 없이 열거한다. 작업 스레드는 기다리지 않는다. 죽고 cover가 완성된 w는
다음 검사에서 없어지고, 미완성은 work로 열린다. 특정 도착 순서/커서/마지막
완료자의 실행 규칙은 없다. OS 감시의 기존 idle wait에 유한 timeout을 주어
검사 기회를 얻는 것이며 자료 준비를 기다리는 단계 루프가 아니다.

## 4. 알고리즘 (제품 코드 전)

- 기본 M=<data>/change-markers, W=<data>/writer-markers. 명시 경로 옵션도 제공.
  표지 stage는 각 root.tmp이며 열거 밖이다. 원본 감시와 두 표지 감시를 함께
  등록한다. Linux 초기 scan은 디렉터리 감시도 등록한다.
- --writer-hook W [--host claude|codex]는 stdin hook JSON을 읽고 조상을 식별해
  원소를 완성 게시한다. 기존 --mark-hook M의 매 append 계약은 그대로 유지.
  직접 시험/통합 명령은 --writer-root W --writer-path f --writer-pid p로 현재
  b를 읽어 게시한다. 임의 생성시각은 제품 입력으로 받지 않는다.
- 평상시 1초 단조시계마다 W만 검사한다. overflow는 M∪W만 검사한다.
  각각 파일 표시는 기존 무순서 비트셋, 은퇴는 별도 순서 없는 검사다.
- native/libvrs에 표지 파서/열거/생존 원시를 두고 데몬과 standalone lib가
  공유한다. lib opts에 복구 콜백과 표지 루트(뒤에 추가)를 넣는다. 콜백은
  준비 비트를 표시하며 단계를 직접 실행하지 않는다.
- 시험 전용(VRS_CC_TEST_METRICS 활성 시만) 요청은 normal notification/periodic
  처리 차단과 명시 overflow 주입을 ACK한다. 주입은 실제 동일 recover 함수와
  lib overflow 함수를 호출한다. sleep 맞추기가 아닌 ACK→append/kill/publish→
  recover→cover 검증이다. 실제 OS overflow 시험도 기존처럼 별도 유지한다.
- 기존 kwov 무표지 fixture는 H 전제에 맞게 작성자 W를 게시한다. 원본/cover/
  발화·SHA oracle·실제 OS overflow·후속 변경 검증은 지우지 않는다. G2 M 시험도
  보존한다. 네 새 경계 시험은 각각 독립 원소와 결정적 제어로 실행한다.

## 5. 경계·대기·출처

새 잠금/큐/순서열/커서/단일 로그/배치 스레드/동료완료 wait 없음.
기존 OS 감시 idle wait는 통지·종료 또는 은퇴 검사 기한만 기다린다.
파일 I/O, 훅 stdin EOF, 프로세스 질의는 OS 호출 완료이며 경합 대기 루프 없음.
구현 후 정확한 파일:줄과 검증 결과를 덧붙인다.

공식 근거(2026-10-13 열어 확인):
- https://learn.chatgpt.com/docs/hooks — 수명주기 입력, 자식 transcript 관련 필드.
- https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes
  — 생성시각 FILETIME 원시 값과 QUERY_LIMITED_INFORMATION.
- https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntqueryinformationprocess
  — 프로세스 기본정보의 부모 식별. 미지원 상태는 실패로 보존한다.

### 4a. 결정적 주입 보강 (시험 코드 전)

실행 중 drop ACK만으로는 이미 시작한 관측과 겹칠 수 있다. 네 경계 시험의
데몬은 시작 환경 VRS_CC_TEST_DROP_WATCH=1로 **초기 스캔 뒤의** 정상 통지와
주기 W 관측을 처음부터 꺼 둔다. test_watch는 그 고정 상태를 확인하는 ACK다.
초기 cover 검증 후 시험 자료를 쓰고 명시 test_overflow를 호출한다. 주입 자체는
제품 recover_watch/recover_overflow와 같은 함수이고 제품 기본 동작은 변하지 않는다.

## 6. 설치 Codex 관측과 훅 알고리즘 확정 (관련 보완 코드 전)

.g3/hooks-3의 Codex0.153.4 실제 실행+localhost 모의 응답은 root/child rollout
2개를 만들었다. root SessionStart/UserPromptSubmit, child SubagentStart 및
UserPromptSubmit가 관측됐다. child의 별도 SessionStart는 없었다. 자식 훅의
session_id는 부모 id지만 transcript_path는 자식이다. 각 경로의 O_RDWR FD를
연 조상 PID가 일치했고 native --writer-hook rc=0으로 W 두 개를 게시했다.
따라서 Codex 설치 예시는 SessionStart/UserPromptSubmit에 SubagentStart도 더한다.
UserPromptSubmit만으로 자식의 초기 게시가 되기 전 공백을 덮었다고 하지 않는다.
기록된 원시 입력·조상 PID/birth/FD flags를 증거로 보존한다.

Windows의 Codex 식별은 조상마다 NtQueryInformationProcess의 ProcessHandleInformation
(51) 결과에서 쓰기 가능한 디스크 핸들만 DuplicateHandle하고 원본 경로를
대조한다. 전역 프로세스/파일 열거 없음. 이 발견 단계는 QUERY_INFORMATION 및
DUP_HANDLE 권한도 필요하며 생존 검사 자체는 QUERY_LIMITED_INFORMATION이다.
권한 상승은 하지 않고 실패는 비영 종료다. Linux 실측을 Windows 실측이라
부르지 않는다. Windows 합성 조상 fixture로 같은 코드를 중계에서 시험한다.

멱등 W 보강은 같은 불변 내용의 표지가 이미 있으면 읽기 확인만 하고 반환한다.
G2 M의 부분 줄 재통지는 바꾸지 않는다. W는 살아 있는 작성자가 게시하는 동안
은퇴될 수 없으므로 이 빠른 길에 lost re-publication이 없다. 죽은 작성자 정보를
임의 입력받는 훅은 없고 테스트의 PID 재사용은 별도 fixture 파일로 모의한다.

## 7. lib Windows 감시 재등록 간극 (L2, G 막음; 수정 전)

플랫폼 vp_watch_tree는 overflow 콜백 뒤 다음 반복에서 감시를 등록했다.
열거 도중의 새 게시가 이 공백에 들어가면 T2의 등록 선행 가정이 깨진다.
이미 완료한 OS 버퍼를 지역 복사하고 같은 OVERLAPPED를 다시 등록한 **뒤**
콜백/경로 처리를 한다. 데몬과 같은 규칙이다. 기다리는 작업자는 추가하지
않고 기존 OS 통지/종료 대기만 유지한다. 판정 데이터에는 영향이 없다.

## 8. g3-001의 시험 관측 간섭 (L4, G 막음: 인증; 수정 전)

c4a0e9a Windows build/static 통과, kwov의 G2 검사에서 max(M 방문)=36으로
기대37과 달랐다. G3가 원본 감시를 다시 켜 정상 처리자가 M 복구 전에 한
표지를 처리·청소할 수 있다. 데이터 손실은 이 실패의 원인이 아니며, 그렇다고
37 기대를 ≤37로 바꾸지 않는다. 기존 G2 시험은 원래 M만 감시하는 조건이었다.
VRS_CC_TEST_METRICS와 VRS_CC_TEST_MARKERS_ONLY를 함께 준 시험에서는 원본
완료 통지만 버린다(Linux는 초기 원본 scan에 감시를 추가하지 않음). M/W의
실제 OS overflow·37 방문·원본 검증·동시 게시 조건은 그대로다. 제품 기본은
세 감시를 모두 유지한다. G3 네 경계 시험은 별도의 결정적 주입 조건 그대로다.

## 9. 세션 열거의 루트 링크 (L2, G 늦춤; 수정 전)

sources는 재귀 열거 안의 symlink를 제외하지만 시작 folder 자체의 링크를
검사하지 않았다. `<session>`이 원본 루트로 연결되어 있으면 재귀 생성자가
그 루트로 들어가 N을 방문할 수 있어 T3 비용 경계가 깨진다. 기존 시작 scan도
링크를 제외하므로 같은 입력 범위에 맞춘다. folder의 symlink_status를 먼저
확인하고 link/비디렉터리는 열거하지 않는다. Windows는 REPARSE_POINT도
명시 검사한다. 결정적 시험의 주요 68/4132·Δ37·W36/source37 기대는 그대로
두고 그 뒤 독립 fixture에서 루트 방향 링크(Linux symlink/Windows junction)를
추가해 원본 하나만 처리되고 재귀 전체 방문은 없는지 확인한다.

비용식의 |D(w)|는 JSONL 개수만이 아니라 그 세션 폴더의 디렉터리/기타 항목도
포함한 열거 항목 수로 읽는다. 기존 sources 계측은 **원본 callback 횟수**다.
고정 주 시험에는 subagents 디렉터리 1개가 추가로 있고 양쪽 크기에서 같다.
표지 진입점36, 원본 callback37, 세션 내부 디렉터리1이며 은퇴 뒤 재확인은
죽은 두 원소 각각의 원본 하나만 추가로 확인한다. 어느 경로도 F를 열거하지 않는다.

타이머 timeout의 ms 변환은 ceil로 한다. floor에서 만료 직전 1ms 미만 동안
0 timeout으로 같은 idle loop가 재진입할 수 있어(기존 자료 대기와는 무관)
시간단위 반올림만 보강한다. 주기/순서/문턱 변경 없음.

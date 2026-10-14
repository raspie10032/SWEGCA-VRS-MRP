# N-B: Windows 동시성 확장 (2026-10-13)

기준: nb 시작점 `57fbff5`, `VRS_MCP_libvrs_v1.md` §13, §14.0–14.10.
`RULES.md`는 작업 트리에 없었고 E 알림 후 `relay/tasks/RULES.md`를 읽었다.
전달된 공통 규칙과 동일하다.

## 1. 정의 (동결)

판단은 frozen lib7과 동일하다. BOrder 순서, observer, 셀, 기여 단위,
조립, 입력, 예산, 오류값을 바꾸지 않는다. 허용 범위는 실행과 스레드
상태의 표현이다. 파일 접근은 A 범위이므로 손대지 않는다.

## 2. 수식 (측정 전에 고정)

T(w) = open 시작부터 ready까지 경과 시간, C(w) = 같은 구간의 프로세스
user + kernel CPU 시간, S = median(T(1)) / median(T(6)).
종전 기준과 수정본은 동일 측정 도구, CPU 집합, 입력, 인덱스에서 비교한다.
개선 조건은 S_new > S_base이며 N1 및 static도 통과해야 한다.
진단 수치는 insert_waits, pass_retries, count_retries, tasks, faults이다.
계측 코드는 준비 완료 후 기존 debug 통계를 읽는다. 이벤트 비용에는
N 항을 추가하지 않는다. 준비 측정 바깥의 stats·close 비용을 섞지 않는다.

## 3. 증명 의무

진단 도구는 lib 판단 상태를 쓰지 않는다. 기존 vrs_wait(NULL)를 사용하면
busy polling 없이 같은 ready 조건을 관찰한다. timeout/error는 실패이며
시간값을 성공으로 보고하지 않는다. Windows GetProcessTimes 및 memory
counters는 관측뿐이다. Linux는 getrusage를 사용한다.

복원과 독립된 thread 상태 접근 최적화는 변수의 초기값, 타입, 수명,
스레드 격리를 보존해야 한다. hazard 저장 표현 변경은 모든 atomic 메모리
순서와 보호/해제 순서를 보존해야 한다. 실제 변경 전에 아래에 그 근거와
알고리즘을 추가한다.

## 4. 알고리즘 (최초: 계측만)

`reopen sessions index 6`은 동일 인덱스를 w6, w1로 각각 연다. 각 open 전후
wall/user/kernel/faults를 읽고 기존 debug 통계를 출력한다. 첫 ready_ms는
중계의 종전 w6 해석과 호환된다. timeout 120초, 성공 ready=1을 확인한다.
CPU는 Linux taskset 0–11, Windows 프로세스 affinity 0xFFF로 제한한다.
vrs_wait는 기존 대기의 호출자이며 새 라이브러리 대기 자리는 없다.

## 5. 검증 계획

- 먼저 main 코드 + 동일 측정기의 `n1_kx2`, `reopen` 기준 수집.
- 후보를 구별한 뒤 설계/증명을 여기에 먼저 추가하고 구현.
- 최종 Windows `static`, `n1_m1`, `n1_kx2`, `reopen`.
- Linux batch/reopen에서 m1 409 및 kx2 1206의 조립 JSON(끝 us만 제외),
  assemble rc 목록, cells_hash를 relay/ref의 동결 출력과 비교.
- 금지한 큐/전역 단계 구조가 원본에 이미 있는 사실은 별도로 명시한다.
  원본 WorkQ와 restore_last를 새 설계라고 주장하지 않는다.

## 6. E-N4-LIB 교정 설계 (코드 전)

**결함 L2, G 막음:** 기존 WorkQ는 enq/deq 순서열을 가진 ring이며
restore_last는 도착 순서상 마지막 완료자가 전체 세션을 나열한다.
두 구조를 그대로 두고 N4를 통과했다고 주장할 수 없다.

정의: 작업은 (종류, 식별자)로 지정한 기존 절차의 한 실행 의무이다.
작업의 도착 순서는 판단 키가 아니다. 원본에서도 여러 일꾼이 임의 순서로
실행하고, push 실패 때 호출자가 즉시 실행한다.

수식: 고정 슬롯 C=2^18개, 슬롯당 atomic uint64 값(0=비었음, task+1),
64개 shard × shard당 64개 표시 낱말 × 낱말당 64개 슬롯.
작업 제출은 hash(task)에서 고정 64곳 CAS, 실패하면 기존 직접 실행.
작업 선택은 shard 64개와 그 표시 낱말(각 64개) 이내 확인, 슬롯 exchange.
표시 낱말의 한 비트만 가져가고 나머지는 실행 전에 돌려놓는다.
따라서 제출 O(64), 선택 최악 O(4096) (모두 고정 상수)이며 저장소/이력 N과 무관하다. 숫자 슬롯은 저장 주소이고
도착 번호/enq/deq가 아니다. 고정 크기는 기존 CAP와 같다.

증명:
- 생산자만 0→task+1 CAS 성공 후 leaf 비트→shard 요약 비트를 OR한다.
- 소비자는 shard 요약의 선택 비트를 내린 뒤 leaf 낱말을 exchange(0)한다.
  그 낱말에서 한 비트를 고르고 나머지는 즉시 OR로 돌려놓고, 슬롯을 exchange(0)
  해서 작업을 얻는다. 슬롯을 실제 비우는 원자 연산 하나만 값을 얻는다.
- 재사용에 의해 오래된 표시가 새 값을 먼저 가져가도 새 값은 이미 완전히
  공개된 작업이다. 뒤늦은 표시는 비어 있는 슬롯을 가리킬 뿐 중복 실행하지
  않는다. OR가 요약 해제보다 앞이면 leaf exchange에 포함되고, 뒤면 다음
  선택에 남는다. 완성된 publication을 잃지 않는다.
- restore 작업은 open 중 고유 슬롯에 미리 놓는다. 이때 생산자가 하나인
  것은 핸들 생성의 사유화된 초기화이며 실행 중 단일 쓰기 주체가 아니다.
  동적 작업 슬롯 찾기 실패는 원본의 직접 실행 경로를 그대로 따른다.
- 각 base 세션의 최초 확인 의무도 open 중 별도 비트 집합에 미리 놓는다.
  일꾼 누구나 restore_left==0을 관찰하면 loaded를 멱등 공개하고 비트 하나를
  가져간다. 완료자가 전체를 나열하거나 다음 실행을 독점하지 않는다.
- 전역 B가 완전해야 저장된 fingerprint를 검증할 수 있는 자료 의존은
  D3/D11에 남는다. 이를 읽는 일꾼은 별도 단계 루프에서 기다리지 않는다.
  독립 작업을 계속하고, 실행 가능한 작업이 없을 때 기존 작업 알림 주소에
  잠든다. 모든 restore 완료는 같은 무조건 알림을 하며, '마지막' 분기는 없다.
  이 자료 의존 자체까지 금지로 해석하면 D3/D11 동결과 양립하지 않는 잔여
  조건으로 보고한다. 단순 명칭 변경으로 없다고 하지 않는다.

알고리즘: 별도 vrs_workset.h에 표시 집합/고정 슬롯을 둔다. worker마다
선택 hint를 지역 변수로 가진다. 모든 일꾼은 동일
선택 알고리즘을 실행한다. 최초 확인 완료 뒤도 매번 maybe_ready를 부르고
어떤 완료자 번호도 검사하지 않는다. 빈 base도 같은 worker 경로를 쓴다.
새 OS 대기 없음: 기존 worker_main의 vp_aw_wait가 같은 sig 주소에서
'실행 가능한 작업 또는 정지 알림'을 기다린다.

검증: 슬롯 재사용/다중 생산·소비 exactly-once 스트레스, Linux frozen
batch/reopen/동시 insert, Windows static/N1/reopen 및 w3/w4를 실행한다.

L3 검토 보정(코드 전): 원본 restore_last는 open 뒤 restore 중 새로 생긴
non-base 세션도 확인했다. 따라서 get_sess의 성공한 세션 공개자는 고유 sg에
대응하는 최초 확인 비트도 공개한다. base는 index=sg-1, 새 세션도 sg-1로
서로 다른 슬롯이다. 이 표시는 저장소 스캔 없이 O(1), 어떤 일꾼도 처리 가능.

## 7. 거짓 공유 분리 실험 (코드 전, 원인으로 확정하지 않음)

복원/최초 확인에서 양보·pass/count retry는 Linux에서 모두 0이었다.
독립 reader의 BOrder::hz(8바이트)와 hz_used(4바이트)는 연속 배열이므로
각각 8명/16명의 슬롯이 한 64바이트 cache line을 공유할 수 있다.
각 조회가 CAS, pointer store, clear를 하므로 서로 다른 슬롯도 invalidation을
일으킬 수 있다. 각 원자 슬롯을 alignas(64) wrapper로 분리한 실험을 수행한다.

수식: HP_N=512 고정, 추가 공간 <= 4×512×64 바이트(두 hazard 계열),
조회/획득/해제의 연산 횟수와 메모리 순서는 불변이다. N 항이 없다.
증명: 값/초깃값/연산/순서가 전부 같고 각 원자 객체의 주소 간격만 변한다.
reclaim은 같은 모든 슬롯을 읽으므로 해제 안전성도 불변이다.
알고리즘: atomic<T>에서 상속한 64바이트 정렬 슬롯 타입을 hazard 배열
4곳에 적용한다. 실험은 B-002(작업 집합만)와 분리해서 비교한다.
단독 결과로 효과가 없으면 이 변경을 원인 해결로 채택하지 않는다.
새 대기 없음.

## 8. FLS 조회 분리 실험 (코드 전)

근거: 사용자 계측의 6,500만 VP_TLS 조회. 소스 win_of는 같은 tl_pass를
한 호출에서 3–4회 읽는다. build_unit/unit_valid/settle_one의 반복문은
PassScope ps가 바로 위에 있어 이미 &ps.p를 안다. FLS 함수 호출이 아니라
이 객체 포인터를 전달하는 것이 Windows의 접근 비용을 제거한다.

정의: Pass는 같은 읽기 구간의 cache와 B 안정성 표지이며 판단식이 아니다.
수식: 반복문 길이 k에서 pass 조회 3k~4k회를 0회로, 그 외 win_of에서
3~4회를 1회로 줄인다. cache 조회/갱신, window 계산, 반복 길이는 동일하다.

증명: PassScope 생성자는 tl_pass=&ps.p, 소멸자는 prev를 복구한다.
세 반복문에서 win_of→window_of→BOrder 조회 경로에는 PassScope 생성,
외부 callback, 재진입이 없다. 따라서 그 구간에서 tl_pass==&ps.p이고,
명시 인자를 받은 helper는 정확히 같은 win map을 읽고 쓴다. 다른 호출자는
진입 때 tl_pass를 한 번 읽어 helper에 넘기며 같은 동일성 증명이 성립한다.
다른 판단 함수/메모리 순서/새 캐시/유효기간은 바꾸지 않는다.

알고리즘: win_in_pass(S,y,Pass*)에 기존 본문을 옮기고 win_of는 현재
pass를 한 번 읽어 전달한다. 세 PassScope 직접 반복에서 &ps.p를 전달한다.
FLS 할당/해제, 스레드 격리, Linux thread 상태는 그대로다. 새 대기 없음.
B-003(패딩까지)와 별도 요청으로 비교하고 감소분만 FLS 접근의 효과로 기록한다.

## 9. 분리 실험 판정과 D 통합 계획 (코드 전)

B-002 작업 집합판: w6 중앙값 3015.1ms, user 14656.2ms, kernel 2718.8ms.
B-003 hazard padding: w6 중앙값 3173.0ms, user 14437.5ms, kernel 3375.0ms.
두 판 모두 yield/pass/count retry=0이다. padding이 준비시간/CPU를 줄였다는
증거가 없으므로 최종판에서 제거한다. 정의/문턱은 그대로다.

D-002(b331a29)에서 할당/버퍼 변경만으로 w6 중앙값 867.5ms였다.
D의 설계 NATIVE_D와 보고를 확인했다. 큰 빈 버퍼의 반복 0채움/메모리
전송이 확장을 가렸다는 후보가 강하다. B 단독 작업 집합과 padding은
해결하지 못했고, D 변경이 이 비용을 제거한다. 이를 B의 개선으로 돌리지 않는다.

다음 알고리즘/검증: B-004(FLS 분리) 결과를 보존한 뒤 padding만 되돌리고,
D의 검증 커밋 b331a29를 git merge한다. 직접 파일 접근 코드는 고치지 않으며
D의 변경 원본/설계/증거는 따로 귀속한다. 도구 충돌은 B의 w6/w1 wall/CPU
및 D의 allocator/memory 진단을 모두 유지한다. 동일 B 측정기로 main과
B+D의 S를 측정하여 실제 확장 회복을 확인한다. 잠금·새 대기 없음.

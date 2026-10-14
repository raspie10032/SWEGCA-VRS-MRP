# NATIVE_F — W6 동시 가시성, 2차

출발: ne/nf `67e4700db4fa21255fa987b47999faf55c80910d`.
RULES.md, ROUND2.md와 §13, §14.0–14.10을 읽었다. L1은 동결한다.

## 1. 정의와 관측 경계 (코드 전)

- v603의 W: 위치 쓰기 완료/닫기 → vrs_insert(path, off, line) →
  vrs_written(path, off). A: vrs_assemble; rc=1일 때만 기존 vrs_wait 후 재호출.
- vrs_insert의 성공은 접수이며 처리된 앞부분 pub까지의 공개 완료가 아니다.
  vrs_written은 디스크에 있는 줄의 사본 해제이며 처리 완료 표가 아니다.
- 기존 vrs_wait는 초기 ready와 계보 준비를, vrs_wait_off는 지정 파일의
  pub >= off를 확인한다. v603는 vrs_wait_off를 부르지 않는다.
- W6 기대값/재시도/시간/입력/300ms 후 진단은 변경하지 않는다.
- L1 판단 함수는 이미 공개된 입력에 대한 선택/계보/셈이다. 그 입력이
  준비되지 않았는데 rc=0인 응답을 허용하는지는 L2 공개 경계 문제로 조사한다.

## 2. 수식과 반례 후보

파일 f의 접수 키 (o,l)의 끝 e=o+l+1, 작업 선점 끝 F_f, 공개 끝 P_f라 둔다.
`CLAIMING` 또는 발표된 line id는 P_f >= e를 함의하지 않는다.
현재 vrs_insert는 lmap[key]!=0이면 즉시 성공하고, assemble은 P_f의 스냅샷을
읽으며 P_f가 해당 접수 키를 덮는지 검사하지 않는다.
후보 반례: watcher가 키를 예약/발표/선점 → 호출자 insert가 중복 성공 →
P_f<e에서 assemble rc0 → watcher가 나중에 pub를 전진. 원자 메모리 순서를
강하게 하는 것만으로 아직 실행되지 않은 처리를 완료시킬 수 없다.
이는 아직 Windows 원인 확정이 아니며, 실제 snapshot 관측과 결정적 교차로 확인한다.

## 3. 측정/증명 의무 → 알고리즘 (코드 전)

1. F-001: 수정 없는 67e4700 w6_500 실패율. 중계가 가져갈 때까지 nf HEAD 유지.
2. 관측 전용으로 조립이 사용한 pub, front와 마지막 insert의 off/end/중복 상태를
   같은 호출 스레드에 기록한다. 시험의 최초 판정은 그대로 두고 실패에만 출력한다.
3. 별도 white-box 도구에서 watcher의 발표와 process_run 사이를 명시적으로
   나눈다. 동일 v603 입력/기대값으로 중복 insert 뒤의 조립을 관측한다.
   스레드 sleep/폴링을 추가하지 않는 결정적 실행 순서이며 합격 시험을 대체하지 않는다.
4. 반례를 얻은 다음 준비 조건 수정의 수식/증명을 여기 먼저 추가한다.

관측 비용: 호출당 O(1), 저장소 파일 수/이력 N 항 없음. 관측 저장소는
호출 스레드별 하나이며 FIFO/전역 순서/주인/잠금/새 대기 없음.

## 4. 고정 검증

Windows w6_500 bad0, static, n1_m1, n1_kx2, w3, w4, w5.
Linux 동결 lib7(sha256 3ca3e5a4...)과 m1/kx2 batch/reopen JSON에서
끝 us만 제거한 바이트, cells_hash 및 rc 목록 동일. Linux v603 통과.
실패는 층과 G 영향으로 기록하고 범위 안에서 수정한다.

## 5. 원인 확정과 L2 수정 (구현 전, 2026-10-13)

- F-001/67e4700: Windows 500회 bad12. Linux 동일 빌드 100회 bad3.
- F-002/a893529 관측판: Windows 500회 bad16. 반송된 처음 세 실패 모두
  f2 `pub=626 front=626 end=872 off=626 dup=1(CLAIMING) sg=5/5`.
  lines25/25, pend0은 조립 뒤 별도 통계 시점의 값이라 공개 완료 증거가 아니다.
- 결정적 `publication_probe.cpp`: op11/op25 × 예약/발표/선점/공개직전 8개
  스케줄 모두 insert0, wait0, wait_off1, assemble0이며 요구 발화 누락.
  미완료 일을 실행하고 pub를 옮긴 뒤 모두 원래 기대 발화를 포함한다.
- **F-PUB (L2 구현의 공개 준비 조건 누락, G 막음):** 키가 존재한다는 사실과
  읽을 데이터가 완전히 공개됐다는 사실을 혼동했다. Windows watcher 지연이나
  allocator 자체가 필요조건이 아니다. 강한 원자 순서도 미실행 작업을 끝내지 못한다.

### 정의 → 수식

유효한 (path,off,line) 접수의 끝 e=off+len+1을 기억하는 원자 값 Q_f를 둔다.
`Q_f = max {e | 해당 파일에 명시적 vrs_insert 접수가 시작됨}`.
max는 위치 기반, 교환/결합/멱등이다. 도착 순서나 전역 일련번호가 아니다.
같은 키의 중복은 같은 원본 줄이라는 기존 API 전제를 유지한다.

공개 준비 조건은 조립 호출이 관측한 q=Q_f와 그 호출의 정확한 PubView.P에 대해
`P >= q`다. 미충족이면 rc1과 빈 출력이다. 조건이 참일 때의 판단 함수는
기존 C.assemble 그대로다. lineage 준비는 기존 VI-11 조건을 그대로 쓴다.
watcher-only 변경에는 명시 접수 q가 없고 기존 공개 prefix 의미를 유지한다.

### 증명

T-F1: insert의 모든 성공 경로(키 예약/발표 중복, 복원된 키, early 포함) 이전에
Q_f를 max(e)로 올린다. 같은 호출자에서 insert 반환 후 시작한 조립은 q>=e를
본다(원자 seq_cst, 호출 순서). 따라서 rc0이면 그 조립의 P>=e다.

T-F2: 기존 advance_pub는 done인 완전한 연속 줄만 P에 포함한다. 그러므로
P>=q이면 접수된 각 줄과 그 앞부분의 위치 셀/카운트는 이미 공개됐다.
L1 선택/내용/예산/계보/셈의 코드와 최종 입력 집합은 바꾸지 않는다.
빈틈을 둔 역순 삽입은 그 빈틈이 채워져야 준비되며, 종전과 같은 결과를 낸다.

T-F3: vrs_wait(path)는 기존 pub_waiters에 먼저 등록하고 progress→Q→P를 읽는다.
P를 옮긴 쪽은 등록 수를 보고 기존 bump_progress를 실행한다. 등록 이후의
전진을 놓치지 않으며, 먼저 전진했다면 Q/P 검사에서 준비됨을 본다.
새 일꾼 대기/루프/작업 순서/마지막 완료자 규칙은 없다. 기존 호출자용
유한 timeout API만 같은 진행 단어를 사용한다. vrs_wait(NULL)는 기존 ready다.

T-F4 비용: 접수당 per-file max CAS, 조립/대기 검사당 원자 두 번 O(1),
파일당 8바이트. CAS 재시도는 같은 파일 경쟁에 따른 갱신 재시도이며 다른
작업 완료를 기다리는 루프가 아니다. 저장소 크기/이력 N 항 없음.

### 알고리즘 → 코드 자리

- Sess에 insert_end 추가. vrs_insert에서 중복 검사보다 먼저 max 갱신.
- read_state에서 q를 먼저 읽고 pub_view의 P<q면 즉시 false(rc1).
- vrs_wait(path)에 같은 준비 조건과 기존 pub_waiters의 RAII 등록/해제.
- 기존 advance_pub의 진행 깨우기를 그대로 이용한다. 새 OS 대기 호출은 없다.
- API 주석에 준비 조건을 명시한다. v603의 입력/기대값/재시도/timeout은 그대로다.

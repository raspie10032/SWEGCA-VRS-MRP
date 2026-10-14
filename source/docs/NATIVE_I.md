# N-I: N과 무관한 파일 핸들 상한

2026-10-13. 출발 nh `0d40fe5`, 가지 ni. 이 절은 제품 코드보다 먼저 쓴다.
L1/변환 바이트/조립/셈/선택/임계값은 동결한다. 결함 I-FD는 L2,
G **막음**: soft NOFILE=4096에서 4,132 transcript의 열기가 실패한다.

## 1. 정의 및 변경 전 수명

| 출발판 파일:줄 | 보관하는 자원 | 목적과 종료 |
|---|---|---|
| native/libvrs/vrs_lib.cpp:1055,1074 | Sess.read_fd | 원본 검증·꼬리·선택 payload, Sess 파괴까지 |
| native/libvrs/vrs_lib.cpp:1056,2600 | Sess.shard_fd | .vx 레코드 append, Sess 파괴까지 |
| src/cc_io.hpp:153,166; src/vrs_ccd.cpp:17 | EntryFiles의 source/output/cover | 원본·C(T)·cover, Entry 파괴까지 |
| native/libvrs/vrs_lib.cpp:3485 | .vx 읽기/매핑 | 복원 함수 안에서 닫힘/해제, 이미 일꾼 수로 한정 |
| src/vrs_ccd.cpp:184,227,300 | 열거·감시 핸들 | 열거 깊이/상주 감시 수에 의존, 파일별 상주 아님 |
| src/vrs_ccd.cpp:118,561 | 파이프/요청 스레드 핸들 | 설정된 연결 일꾼 수에 의존 |

Windows도 같은 Sess/Entry 필드를 사용한다. 따라서 파일당 최대 5개의
상주 핸들 구조는 양쪽 모두 같다. 모든 Entry가 실제 다섯 개를 연다는 뜻은 아니다.

정의: 핸들은 파일 바이트 판단의 입력이 아니라 OS 객체에 접근하는 수단이다.
작업 임대(lease)는 동기 I/O를 수행하는 지역 범위 동안 열린 객체를 보존한다.
캐시는 핸들의 임대 사이 재사용만 담당한다. 캐시 배치가 실행/출력 순서를
정하지 않는다. 각 파일의 위치 쓰기와 .vx append 의미는 그대로다.

## 2. 수식과 정리

고정 용량 C=8을 코드와 시험 전에 정한다. 스레드별로 lib와 변환기 각각
한 개의 직접 대응 표를 둔다. 키는 생존 토큰 객체이며 칸은 주소 해시 mod C다.
파일 수나 최근 접근 시각, 도착 번호, 첫/마지막 완료자에 따른 우선순위는 없다.
한 키를 어느 스레드든 열 수 있으며 파일별 지정 소유자/작성자는 없다.

A_l/A_c=각 표를 사용한 살아 있는 스레드 수, L=표 밖에서 사용 중인 지역
임대 수, O=열기 중 후보 수, B=감시/열거/통로/매핑 등 다른 OS 핸들 수이면

    H <= C (A_l + A_c) + L + O + B.

L/O는 동시 호출/스택 깊이로 제한하며 파일/작업 목록에는 임대를 보관하지 않는다.
데몬에서는 A_c<=W+K, A_l<=2W+K, 기존 W+K<=64다. 캐시 항의 최댓값은
8(3W+2K)<=1528(W<=63,K>=1)이다. 외부 lib 사용자의 스레드 수는 별도
동시성 인자이며 N이 아니다. 라이브 호출 깊이별 임대 수는 코드 후 검토에 적는다.
열거의 깊이 인자 B도 파일별 상주와 구분해 보고한다.

T-I1(수명): 표의 각 값과 호출자 임대는 일반 shared_ptr 사본이다. 표 자체는
스레드 지역으로 공유되지 않는다. 칸 교체는 그 표의 참조만 놓는다. 사용 중인
임대가 존재하면 핸들이 닫히지 않는다. 공유 atomic<shared_ptr>는 사용하지 않는다.
표가 키 토큰도 보존하므로 객체 주소 재사용이 다른 Store/Entry의 핸들로 오인되지 않는다.
스레드 종료는 FLS/기존 TLS 소멸자로 표를 닫는다. ∎

T-I2(무직렬): 다른 스레드의 표/임대를 읽거나 기다리지 않는다. 충돌은 정해진
한 칸의 참조 교체이며 순위 비교/순회/대기/작업 위임이 없다. 같은 파일의 여러
핸들은 위치 읽기·멱등 위치 쓰기·OS append를 독립 실행한다. 표의 시간에 따른
내용 변화는 hit/miss 비용에만 영향을 주며 자료 판단/실행 허용 조건에는 영향0이다. ∎

T-I3(비용): 키 칸 조회/교체 O(1), miss의 OS 열기/닫기 O(1) 호출이며 N 순회0.
한 꼬리의 여러 읽기와 한 조립의 근접 항목 읽기는 열린 핸들을 재사용한다.
복원 validate와 두 ingest는 같은 키/스레드 표를 재사용한다. 캐시 miss율에 따른
성능은 별도 실측한다. M8의 5초 문턱을 바꾸지 않는다. ∎

T-I4(파일 정체): 캐시 축출은 원본 교체 허용이 아니다. source의 생존 토큰에는
OS 파일 정체를 작은 불변 값으로 보존하고 새 핸들을 그 값과 비교한다.
기존 경로/삭제 검사도 유지한다. 정체 값에는 OS 핸들이 없다. 토큰의 CAS는 같은
파일 정체 사실을 공개할 뿐 실행 담당자를 정하지 않으며 기다리는 경로가 없다. ∎

T-I5(.vx): 새 append 핸들은 사용 전에 기존 R_WM(0)을 쓴다. 반복 mark의
결합은 기존 max라 의미가 같다. Sess에는 핸들 대신 'append가 시작됨'이라는
bool만 둔다. 기존 mark_wm의 준비 조건은 이 사실을 사용한다. 캐시 축출이 WM
최댓값을 지우지 않는다. 삭제/재구축은 restore 중 append 전에만 일어나고 새
Store의 생존 토큰은 다르므로 이전 삭제 객체의 핸들을 재사용하지 않는다. ∎

## 3. 알고리즘

1. 재사용 가능한 C칸 TLS/FLS 표와 shared 임대 형식을 정의한다. miss는 한 칸을
   선택하고 직접 열어 반환한다. 실패는 캐시에 남기지 않는다.
2. Sess의 두 fd를 핸들 없는 키로 바꾸고 source/shard 함수가 지역 임대를 돌려준다.
   fetch/validate/ingest/shard_write는 I/O가 끝날 때까지 임대를 지역 변수에 둔다.
3. FileSlot을 파일별 OS 핸들 대신 정체 토큰으로 바꾼다. get은 임대를 반환한다.
   cover·prepare·publish·work·restore가 사용하는 동안 지역 임대를 둔다.
   Tail/Group/Entry에 열린 핸들을 넣지 않는다.
4. 기존 위치 읽기/쓰기, cover 읽기 전 C(T) 읽기 순서, 완료 뒤 cover 쓰기,
   명시 입력 공개 준비 조건과 감시 복구 알고리즘은 유지한다.
5. 새로운 대기0. 기존 자기 I/O 완료 대기는 유지하며 최종 파일:줄을 기록한다.

## 4. 고정 검증 조건

- soft NOFILE=4096, G와 같은 32/4096 dormant + active32 + new4 조건
  (최종68/4132 transcript)에서 원래 실제 overflow 및 N5를 검사한다.
- 두 크기에서 Linux /proc/<pid>/fd, Windows GetProcessHandleCount를 주기적으로
  관측한다. 표본 최댓값은 완전한 연속 관측 최대와 구분한다. fixture W=6,K=4,
  동일 디렉터리 깊이에서 총 핸들 관측 상한 **512**를 실행 전에 고정한다.
  이 상한은 캐시 8*(3*6+2*4)=208, 지역 임대·후보·기존 OS 자원의 여유를 포함한다.
- 데이터/rc/hash/JSON 기준과 timeout/NOFILE/Windows Defender 설정은 변경0.
- relay: static n1_m1 n1_kx2 reopen w5 w6_500 samples kx4 kwov.
  kx4 M8 5회 모두<=5초. Linux 동결본 N1와 변환/동시성/정체 회귀를 실행한다.
- 넘침 전체 스캔의 비용 결함은 G2 담당이다. I가 N5 통과를 비용 해결로 보고하지 않는다.

OS 정체 근거: [FILE_ID_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_info).
계측 근거: [GetProcessHandleCount](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesshandlecount).

## 5. 구현 뒤 검토와 Linux 관측 (Windows 요청 전)

지역 임대는 fetch 1, validate 1, ingest 1(+중첩 shard_write 1), shard_write 1,
cover 읽기/쓰기 각1, prepare source+output 2, publish output1(+cover1 또는
vrs_insert의 shard1), restore source1(+assembly fetch1)이다. 이 경로에서 핸들은
Tail/Group에 이동하지 않는다. 각 활성 스레드의 표 밖 보수적 임대 상한4,
새 open 후보1이다. 따라서 fixture W6/K4에서 표208+지역80+후보16에
기존 감시·매핑·통로·FLS 사건 등을 더해 사전 고정 총512를 검사한다.
Directory 열거 핸들은 파일당 유지가 아니라 동시 열거 깊이 D에 의존한다.
깊이/경로 길이를 고정한 두 규모를 비교한다. 임의 깊이 트리의 전체 자원이
상수라는 주장을 하지 않으며 경로 길이 제한/동시 API 호출 수는 별도 인자다.

Linux .i-artifacts/kwov: soft4096/hard1048576, 원래36개 및68/4132개
모두 실제 IN_Q_OVERFLOW1, rescan1, cover=EOF, N5 통과. fd 5ms 표본
최댓값122/116/127, 모두 같은512 안. N 의존 full scan은 유지(G2 담당).
N1 m1/kx2 batch/reopen의 JSON/rc/hash 동일. W6 500 bad0.
handle_cache_test: 실제 한 칸 충돌 중 임대 읽기, cache hit 재사용, 축출 후
원본 교체 거부, 8스레드8000회 위치 쓰기/읽기 통과.
새 제품 대기0. 계측 스레드의 5ms Event.wait와 종료 join은 시험 도구에만 있다.

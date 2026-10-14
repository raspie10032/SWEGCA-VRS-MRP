# H2 최종 통합 설계와 사전 판정 기준

2026-10-13. 출발 nh 0d40fe5, I 952d3c4. 제품 변경 전에 기록한다.
보호 명세 §13, §14.0–14.10 및 RULES/ROUND2의 L1과 문턱은 바꾸지 않는다.

## 정의

H2는 이미 정의된 변환/조립 판단을 보존하면서 I의 지역 핸들 임대와
G2/G3의 변경 관측·복구를 합친다. N1–N5는 원래 시험 그대로 유지한다.
G3 통과 커밋은 relay/notes/h2.md의 명시적 인계 뒤 합친다.
그 전 ng2 518dbe5는 시험 가지 nh2-g2-trial에서 충돌을 사전 해결한다.

## 수식과 의무

- I: H <= 8(A_lib + A_conv) + L + O + B. 파일별 상주 핸들 N 항은 0.
- G2: T_recover = O(|M| + 경로 길이 합 + 변경 꼬리 + 기존 work/lib 비용).
  G2 단독은 모든 append의 표지 게시 전제가 있어 전체 비용 완료가 아니다.
- G3: 실제 통과 설계의 작성자 수명 및 데몬/lib 복구 비용을 각각 검토한다.
  작성자 집합·변경 집합 이외에 이력 파일 수 N을 열거하면 미통과다.
- N2: M8 다섯 값 모두 <= 5초. W6: 500회 bad 0. 핸들 실험 상한 512.
  N5 원래 36개와 I의 68/4132개, G2/G3 확장 시험을 모두 유지한다.

## 병합 보존 증명

1. G2의 File(HANDLE) 생성자는 검증된 OS 핸들의 소유권만 받는다.
   I의 FileSlot 생존 토큰/직접 대응 캐시 및 shared 임대를 대체하지 않는다.
2. work/restore/clean_marker에서 File 참조를 사용하는 전체 구간 동안
   지역 shared_ptr 임대를 보존한다. 캐시 충돌에도 File의 수명이 끝나지 않는다.
   임대를 Entry/Tail/Group에 저장하지 않아 파일당 핸들 상주를 되살리지 않는다.
3. 표지 청소는 기존 cover의 사실만 읽고, 삭제 후 원본 크기를 재확인한다.
   이는 조립·선택·토큰 셈에 입력값을 추가하거나 기존 값을 바꾸지 않는다.
4. kwov의 기존 N5/핸들 검증과 표지 검증의 성공을 논리 AND로 합친다.
   한 검증의 실패를 다른 검증의 성공으로 덮지 않는다.

## 알고리즘

I를 git merge --no-ff로 합친 nh2 c29c9ea에서 H2-001 전체11시험을 요청한다.
요청 결과가 돌아올 때까지 nh2를 고정하고 별도 시험 가지에서 ng2를 merge한다.
충돌은 위 수명·청소·검증 의무를 함께 유지하도록 국소 수정한다.
Linux 빌드/N1/W6/회귀와 사전 병합 검증을 CPU0–11에서 수행한다.
G3 통과 커밋을 합친 최종 판은 Windows 전체11시험과 Linux 검증을 다시 한다.
각 결과의 실제 rev와 바이너리 hash를 기록한다. 모든 조건 충족 때만
main에 git merge --no-ff nh2 및 native-pass 태그를 수행한다.

## 대기와 결함 분류

이 병합으로 새 제품 대기 자리를 만들지 않는다. 기존 OS I/O, 상주 일꾼,
감시/종료, 명시 API timeout 대기는 최종 소스 줄 번호로 보고한다.
실패는 L1/L2/L3 및 G 막음/늦춤/무관으로 갈라 해당 층에서 수정한다.
범위 밖 전제는 보고하되 다른 검증은 계속한다.

## 사전 병합에서 확인한 결함

- H2-M1, L3/G 막음: kwov 자동 병합이 G2 run_suite를 I run_case 내부에
  넣어 정의되지 않은 args를 참조한다. suite는 main의 세 I case 뒤 한 번
  실행하고 최종 pass에 AND한다. false는 프로세스의 비영 종료로 전달한다.
- H2-M2, L2/L3/G 막음: G2 clean_marker의 임시 임대가 문장 끝에 사라진다.
  cover_max가 같은 캐시 칸을 교체해도 source가 살아 있도록 지역 임대를 둔다.
- H2-M3, L4/G 판정 막음: G2 시험이 NOFILE을32768로 올리던 코드가 I의
  고정4096 의무를 깨뜨린다. 시험 시작에서4096을 넘으면 낮추기만 하고,
  G2 두 크기에도 I의 핸들 표본 계측/512상한을 동일하게 적용한다.

## G3 사전 통합 (2026-10-13, 코드 해결 전)

nh2는 I 통과판에 고정한다. 별도 nh2-g3-preview에서 G3 f607d73을 미리
merge해 겹침을 검증한다. G3 통과 인계 전에는 이 미리보기를 nh2에 합치지 않는다.

G3가 정한 입력 전제는 살아 있는 작성자의 수명 표지 W이다. I의 기존 세
N5 조건은 원본 바이트·36/68/4132개·실제 OS 넘침·cover/발화/SHA/본문
기대값을 유지하며, 변경할 파일에만 W를 게시한다. 처음32개, 변경할 조용한
파일1개, 새파일4개가 대상이다. 정적 이력 전체에 표지를 만들지 않는다.
새 파일 생성 전에도 W를 게시할 수 있으므로 생성 통지 손실과 무관하게 포괄한다.

G3의 두 규모 시험에도 I와 같은 NOFILE4096 및 HandleSamples512를 적용한다.
case의 성공과 핸들 상한을 AND해 suite 및 최종 종료값까지 전달한다.
G3의 네 결정적 주입/딸린파일/은퇴/훅 검증과 G2의 고정37 방문 기대값은 보존한다.
원본 통지를 차단하는 G3 시험 설정은 G2의 표지 전용 복구를 격리하기 위한
실행 전 주입 조건이며 제품 기본 감시나 검증 기준을 낮추지 않는다.

중계는 kwov 출력 끝3000자만 돌려준다. 전체 result.json은 보존하고,
stdout은 세 N5 조건·두 G2/G3 조건의 핵심 수치와 성공 여부를 요약한다.
표본 최대와 수명 상한을 구분하며 이 요약은 기대값/timeout을 바꾸지 않는다.

## H2-CALLBACK-LIFE — 종료 수명 (L2/L3, G 막음)

G3가 lib watcher에서 Daemon::recover_watch(this)를 호출하도록 바꾸었다.
기존 종료는 Resident join→Resident 파괴→vrs_close(lib watcher join)다.
콜백이 stopping_==false를 이미 읽고 지연되면, Resident 파괴 뒤 mark에서
해제된 객체를 사용할 수 있다. stopping_의 시작 검사만으로는 기존 콜백을 취소하지 못한다.

수명식: last(Resident의 lib 사용) < vrs_close 시작;
last(lib watcher의 Resident 사용) < Resident 파괴.
둘을 충족하는 종료는 기존 Resident join→기존 vrs_close→Resident 파괴다.
Resident의 lib 참조는 join으로 끝나고, lib 콜백은 vrs_close의 기존 watcher
join으로 끝난다. 콜백이 늦게 bit를 표시해도 Resident 객체는 살아 있으며
stopping 상태에서 실행을 재개하는 규칙을 추가하지 않는다. 종료 뒤의 원본
복구는 기존 재시작 초기 스캔 계약이다. 새 join·잠금·준비 대기는0이다.
이 종료 수명 순서는 이벤트의 전역 순서나 정상 처리 단계 대기가 아니다.

알고리즘: resident_.reset()만 vrs_close 뒤로 옮긴다. 판단 함수/자료는 그대로다.

## W5 실패 근거의 반송 (L4 관측 보강, 제품 변경0)

G3-003의 W5는 pass=false지만 중계 요약에서 reopen200·최종 원본·hash는 같다.
고정 W5 스크립트는 각 trial의 kill 전 ready/alive 및 kill_rc도 검사한다.
중계는 그 필드를 돌려주지 않는다. kwov가 같은 요청 폴더의 w5root/work에
있는 trials.jsonl과 실패 trial의 kill 로그를 읽어 result.json과 간략 stdout에
추가한다. 파일이 없으면 이 추가 관측만 생략한다. W5 판정·fixture·실행기는
변경하지 않는다. 다른 요청 폴더나 외부 데이터는 읽지 않는다.

후속 진단 범위: g3-003의 W5 실패가 요약에서 설명되지 않아, 그 지정 시험의
보존된 trials.jsonl 및 해당 kill 로그만 추가로 읽어 반송한다. 요청 id는
g3-003으로 고정하고 모두 읽기 전용이다. 다른 자료/자격증명/시험 상태를
읽거나 바꾸지 않는다. 과거 실패를 재실행 성공으로 지우지 않기 위한 근거 수집이다.

## 최종 자원·수명 검토


I 일반식 H <= 8(A_lib + A_conv) + L + O + B는 유지된다. G3는 데몬 watcher와
lib watcher 콜백에서도 cover_max(FileSlot)를 사용하므로 변환기 캐시 참여자
상한을 A_conv <= W+K+2로 기록한다. A_lib <= 2W+K이며, 보수적으로 callback
진입자의 향후 lib 캐시 접근까지 포함하면 A_lib <= 2W+K+2를 쓸 수 있다.
따라서 캐시 상한은8(3W+2K+4), fixture W6/K4에서는240이다. 원래I의208을
그대로 복사해 G3 새 참여자를 누락하지 않는다. 실제 호출 그래프의 tighter
값224와 구별한 보수적상한이다.

L/O는 활성 스레드별 지역임대/자기 open후보, B는 고정 watcher3의 dir/event,
기존libwatcher, pipe/stop/FLS I/O event, 동시열거깊이/지역프로세스질의다.
표지와원본파일을순회하면서이전File은범위끝에서닫는다. W원소나Entry에
OS프로세스/파일핸들을상주시켜두지않는다. 어떤항에도저장소파일수N이없다.
동시API호출수/설정된스레드수/동시열거깊이는별도인자다. fixture 관측상한512는
바꾸지않는다. 5ms표본최댓값을절대최대나수명증명자체로취급하지않는다.

G3의콜백추가로파괴순서는Resident작업join→lib종료(join watcher)→Resident파괴다.
이순서는lib를사용하는Resident작업과Resident를사용하는lib콜백의수명을동시에
보존하며정상사건순서/처리담당자를정하지않는다.새대기호출은없다.

vrs_opts 확장: 저장소의CLI/insert/conc/reopen/w5/v603/daemon 모두vrs_opts o{}로
초기화한다. 추가필드는기존판단인자를변경하지않는다. 정적archive클라이언트는
확장헤더로재빌드해야하며옛ABI바이너리호환을주장하지않는다.

G3 인계: relay/notes/h2.md의 통과 커밋6e9557d 및 g3-004 rev를 대조했다.
H2 미리보기의 git merge 계보에 그 정확한 커밋이 포함됨을 확인한 뒤 nh2에
merge --no-ff했다. G3 진단용 fef4b1f는 합치지 않고 검증 제품 커밋을 사용한다.
H2-003은 추가 W5 원시 진단 및 결합kwov, 최종 전체 요청은 별도 H2-004다.

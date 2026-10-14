# KL3: Windows 파일 API 경계의 긴 경로 (2026-10-13)

출발점: `nk3`, `7a200cc`. L1/lib7 판단은 동결한다. 결함은 L3,
G 영향은 막음이다. MAX_PATH를 넘은 원본/파생 경로의 열기 실패가
`watch_error 5` 및 `WATCH_INCOMPLETE_5`로 전파된다. 시스템 설정은 바꾸지 않는다.

## 1. 정의

- P: 내부 UTF-8 경로. 열쇠, 상대 경로, 계보, cc 위치, JSON과 색인에
  쓰는 표현은 종전과 같다. `vp_path`는 UTF 변환 및 순수 경로 연산용이다.
- A(p): Windows의 현재 디렉터리를 기준으로 구한 절대 경로. 구분자를
  역슬래시로 바꾸고 `.` 및 `..`를 해석한다. 파일 존재를 요구하지 않는다.
- E(p): 파일 API 전용 UTF-16 경로. 이미 `\\?\` 또는 `\\.\`이면 그대로;
  그 밖에는 A(p)가 드라이브 경로이면 `\\?\`를 앞에 붙이고,
  UNC이면 선두 `\\`를 `\\?\UNC\`로 바꾼다. 빈 경로는 빈 경로로 둔다.
- D(p): 파일 API가 반환한 경로에서 DOS 확장 접두를 제거한다.
  `\\?\UNC\s\t`는 `\\s\t`, `\\?\C:\t`는 `C:\t`다.
  UTF-8 문자열 출력에서는 구분자를 `/`로 맞춘다. 장치/파이프 입력은
  E에서 손대지 않는다.
- 파일 API 경계: Win32 이름 인자, std::filesystem의 파일 접근 인자,
  C/C++ 파일 스트림 이름이다. 파일명 추출과 path 비교는 이 경계가 아니다.

## 2. 수식 및 정리

T1(지칭 보존): 보통 파일 p에 대해 object(E(p)) = object(A(p)).
절대 경로와 점 성분을 먼저 구하므로 확장 경로에서 상대 성분을 해석하지
않는 Windows 규칙과 충돌하지 않는다. UNC도 서버/공유/나머지 성분이 같다.
이미 이름공간을 지정한 입력은 항등이므로 파이프 지칭도 보존된다.

T2(표현 보존): D(E(p))는 A(p)의 종전 내부 표현이다. 디렉터리 열거와
realpath의 출력에 D를 적용하고, 정렬/비교에는 P만 사용한다. 따라서
접두가 rel, fid seed, 경로 순서, cc 위치 또는 JSON에 들어가지 않는다.
파일 바이트와 lib7 알고리즘은 바꾸지 않으므로 같은 입력의 판단은 같다.
실측 증거는 고정 동결본 N1의 JSON(끝 `us`만 제외), rc 목록, cells_hash다.

T3(비용): 변환 비용은 O(|p|), 임시 공간 O(|p|)다. 저장소 파일 수 N이나
이력 길이를 읽거나 순회하지 않는다. 공유 가변 상태, 도착 순서 규칙,
추가 일꾼, 새 대기는 없다. 기존 감시/복구가 건네는 각 이름에도 같은
독립 변환을 적용하므로 넘침 복구의 N5 및 기존 비용 경계를 보존한다.

## 3. 알고리즘 (코드 전 고정)

1. Windows 전용 `vp_io_path`를 추가한다. UTF-8 → UTF-16 후 이름공간
   접두를 판별하고, 보통 경로는 `GetFullPathNameW`의 크기 질의/결과로
   A를 얻어 E를 만든다. 고정 MAX_PATH 버퍼를 쓰지 않는다.
2. lib와 데몬의 모든 파일 접근 인자를 이 함수로 바꾼다. 기존 `vp_path`
   비교/filename/extension은 유지한다. Linux에서는 새 이름을 기존
   `vp_path`의 별칭으로 두어 경로 구현과 실행 의미를 그대로 보존한다.
3. `vp_u8` 및 realpath 결과를 D로 되돌린다. 핸들의 최종 이름을 비교하는
   source identity 검사도 같은 표현끼리 비교한다. 열거된 fs::path를
   내부에 보관할 때는 먼저 D로 되돌린다.
4. 고정 MinGW 중계에서 `create_directories`, `directory_iterator`,
   `recursive_directory_iterator`, `canonical`, `weakly_canonical`,
   `file_size`를 300자 초과 경로로 실측한다. 성공/오류를 개별 기록한다.
   실패한 파일 시스템 기능은 L3로 돌아와 Win32 구현으로 대체한다.
5. `tools/longpath_test.py`는 Windows 전용이다. 고정 samples 4개를
   바이트 그대로 복사하고 출처/sha256을 기록한다. 원본과 data를 각각
   깊게 두며 원본·C(T)·cover·idx 경로가 300자를 넘음을 검사한다.
6. restore의 항목 수 66/0/167/17과 각 원본 위치·sha·본문(M2)을 검사한다.
   kwov와 같은 64바이트 감시 버퍼로 실제 넘침을 일으키고, writer witness를
   통해 변경이 restore 도움 없이 변환됨을 검사한다. 실제 넘침 관측이 필수다.
7. 같은 자료의 짧은 경로 실행과 긴 경로 실행을 비교한다. C(T)는 바이트
   그대로, 조립 JSON은 끝 `us`와 두 실행의 루트 경로 치환만 허용한다.
   lib CLI의 일괄/다시 열기도 긴 경로에서 검사한다.

Microsoft 근거: [최대 경로 길이](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation),
[GetFullPathNameW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfullpathnamew).

## 4. 고정 끝 조건

- 중계 `longpath` 통과.
- `static n1_m1 n1_kx2 reopen w3 w4 w5 w6_500 samples kx4 kwov` 통과.
  W6 500회 bad 0, M8 5회 모두 5초 이하.
- Linux 빌드 성공, M1 409개/kx2 1,206개 일괄·재열기 판단 동결본과 같음.
- 금지 기호/잠금/큐 추가 0, 새 대기 0. 기준/기대값은 결과 후 변경하지 않는다.
- 보고는 `$HOME/tmp/vrsnative/REPORT_K3.md`, 통과 커밋은
  `relay/notes/k3.md`. main에 합치지 않는다.

## 5. 실측 및 결함 기록

### k3-001 (`96af3bb`): MinGW 경계 결함, L3, G 막음

Windows 빌드/정적 검사 통과. 300자 초과 + 한글 경로에서
`create_directories`, `file_size`, 스트림, 핸들 정체 검사는 통과했다.
`directory_iterator`, `recursive_directory_iterator`는 기대 항목을 반환하지
않았고 `canonical`은 존재하는 파일을 ENOENT로 보고했다.
`weakly_canonical`도 경로가 다르다. 접두 변환 자체는 통과했다.
따라서 std::filesystem에 접두만 주는 구현으로는 KL3를 닫을 수 없다.

대체 알고리즘 (다음 코드 전에 고정):

1. Windows 전용 디렉터리 반복자를 `FindFirstFileExW/FindNextFileW`로
   구현한다. 순회 스택은 호출마다 독립이며 디렉터리 깊이만큼의 검색
   핸들을 갖는다. 재귀 진입 시점을 제외하면 항목 하나당 다음 찾기 1회다.
   이것은 이름 열거의 파일 시스템 커서이며 작업/요청 대기열이 아니다.
   파일 크기/종류는 WIN32_FIND_DATAW에서 얻는다. 링크를 따라가지 않는다.
   에러 코드와 skip_permission_denied를 보존한다. 내부 반환 path는 D를 쓴다.
2. `canonical`은 이미 있는 Win32 realpath로 대체한다. 존재하지 않는 파일의
   writer witness에는 정상화한 절대 이름의 가장 긴 존재 부모를 핸들로
   canonicalize하고 미존재 suffix를 붙인다. 부모 탐색은 경로 성분 d에 대한
   O(d) 메타데이터 호출/O(d |p|) 문자열 비용이고 저장소 N과 무관하다.
   존재하는 경로의 일반 경계 변환은 종전 T3의 O(|p|)다.
3. Linux는 반복자와 weak canonical의 기존 구현을 그대로 사용한다.
4. 원래 std::filesystem의 실측 결과를 계속 기록한다. 통과 여부는 실제로
   사용하는 Win32 대체 구현의 동일한 기대값 검사로 결정한다. 원래 함수의
   실패를 성공으로 표시하지 않는다. 표본/N1/M2/넘침 기준은 그대로다.

### k3-002 (`6483a6a`): 긴 경로 통과

- Windows `LongPathsEnabled=0`, 빌드/정적 검사 통과.
- 원본 최소 436자, 파생 cover 최소 438자. 짧은 경로는 각각 128/135자.
- 긴/짧은 각 표본 66/0/167/17, 각 250항목 원본 위치/sha/본문 일치.
  C(T)는 고정 stage/data5와 바이트로 같고, 양쪽 restore 및 조립도 같다.
- 양쪽 cells_hash `e3f1dc4c2115d5d5`, CLI 일괄/재열기 모두 같다.
- 실제 넘침 34회, 복구/후속 변경 24줄 모두 원본 대조 통과, 복구 중 restore 0회.
- std 함수 네 개는 계속 실패로 기록하고 네 Win32 대체 검사는 모두 통과한다.
- 후속 검증은 동일 알고리즘에서 UNC 표지의 대소문자와 슬래시로 쓴
  namespace 접두도 검사한다. UNC 서버 접근은 별도 서버가 없어 문자열
  변환만 검사하며, 실제 긴 파일 접근 증거는 로컬 NTFS다.

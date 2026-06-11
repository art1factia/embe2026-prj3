# Cosmos+ OpenSSD Project #3 — Key-Value Interface 개통 및 KV-SSD 펌웨어 구현 (보고서 초안)

> docx 템플릿(`prj3-document.docx`)에 항목별로 붙여넣어 사용하세요.
> `[캡쳐]` 표시는 보드 연결 후 실제 실행 화면으로 교체해야 하는 자리입니다.

---

## 1. 프로젝트 개요 및 목표

본 프로젝트의 목표는 Cosmos+ OpenSSD 플랫폼의 기존 블록 인터페이스 펌웨어(GreedyFTL) 위에 **Key-Value 인터페이스를 개통**하고, 호스트가 발행하는 NVMe KV-PUT/KV-GET 명령을 올바르게 처리하는 **최소 기능 KV-SSD 펌웨어**를 구현하는 것이다.

Task 흐름은 다음과 같다.

1. 호스트 측 `kv_bench` / `nvme_passthru.{h,cc}` 분석 → 본 과제에서 채택한 NVMe KV 명령 포맷(opcode, DWORD 배치, 완료 규약) 파악
2. 펌웨어의 NVMe I/O 디스패치 경로에 KV opcode(0xA0/0xA1) 핸들러 연결
3. 디바이스 내부 인덱스 자료구조(해시 테이블) 및 Value 저장 관리 기법(논리 주소 공간 위 slot 매핑) 설계·구현
4. `kv_bench`로 정합성 검증 — 모든 GET이 "가장 최근에 PUT된 값"을 반환하고(FAIL=0), 존재하지 않는 키에 대해 No-such-key 시맨틱이 동작해야 함

## 2. Task 1 — Key-Value Interface 개통 및 KV-SSD 펌웨어 개발

### 2.1 NVMe KV I/O Command와 기존 Block I/O Command의 차이

#### 명령 포맷 비교 (본 과제 채택 포맷)

| 필드 | Block Write (0x01) / Read (0x02) | KV-PUT (0xA0) | KV-GET (0xA1) |
|---|---|---|---|
| CDW10–11 | 시작 LBA (64b) | **Key (4B)** (CDW10) | **Key (4B)** (CDW10) |
| CDW12 | NLB (블록 수 − 1) | NLB (=0, 4KB 고정) | NLB (호스트 버퍼 크기 기준 3) |
| CDW13 | DSM 등 | **Value 크기 (4096)** | 수신 버퍼 크기 (16384) |
| 데이터 | PRP가 가리키는 호스트 버퍼 | Value (RxDMA) | Value (TxDMA) |
| CQE DW0 | (미사용) | **0 이어야 성공** | **반환 Value 길이 (4096)** |
| 상태 코드 | Generic status | SUCCESS | SUCCESS 또는 **No-such-key (SCT=0x7, SC=0xC1 → 호스트 가시값 0x7C1)** |

#### 주소 지정 모델의 차이 (시각 자료 — 그림으로 옮길 것)

```
[Block Interface]                         [KV Interface]
 App                                       App
  |  read(file, offset)                     |  GET(key)
  v                                         v
 Filesystem (파일→LBA 인덱싱: extent,      KV Library (얇은 passthrough)
  inode, journal 관리 부담)                 |
  |  LBA, NLB                               |  key
  v                                         v
 NVMe Block Cmd (0x01/0x02)                NVMe KV Cmd (0xA0/0xA1)
  |                                         |
  v                                         v
 SSD FTL: LBA→PPN 매핑                     SSD: key→위치 "인덱싱 자체가
  |                                         |    디바이스로 이동"  + FTL
  v                                         v
 NAND                                      NAND
```

#### 호스트 소프트웨어 스택에 미치는 영향

- **인덱싱 책임의 이동**: 블록 인터페이스에서는 "이 데이터가 어느 LBA에 있는가"를 파일시스템/DB 엔진이 B-tree, extent map 등으로 관리한다. KV 인터페이스는 이 매핑을 SSD 컨트롤러 내부 인덱스로 흡수하므로, 호스트는 키만 제시하면 된다. 호스트 스택(파일시스템 저널링, 페이지 캐시 동기화, 이중 매핑)이 얇아지고, KAML[1]이 보인 것처럼 트랜잭션·캐싱까지 디바이스로 내릴 여지가 생긴다.
- **이중 변환 제거**: KV 스토어(LSM-tree 등)를 파일시스템 위에 올리면 "key→file offset→LBA→PPN"의 3중 변환과 이중 쓰기 증폭(WAL + 저널)이 발생한다. KV-SSD는 "key→슬롯(논리 주소)→PPN"으로 줄여 데이터 이동과 쓰기 증폭을 감소시킨다(iLSM-SSD[2], BandSlim[4]).
- **인터페이스 시맨틱 변화**: 고정 크기 섹터 배열이라는 추상화가 가변 키-값 컬렉션으로 바뀌므로, 오류 모델도 "LBA out of range"가 아니라 "No such key" 같은 키 단위 시맨틱으로 바뀐다. 본 과제에서는 이를 vendor-specific status(0x7C1)로 표현했다.

### 2.2 KV 명령 경로 개통 (인터페이스 enable)

`nvme/nvme_io_cmd.c`의 `handle_nvme_io_cmd()` 스위치에 `NVME_CMD_KV_PUT(0xA0)`/`NVME_CMD_KV_GET(0xA1)` 분기가 추가되어 있으며(스켈레톤 제공), 각 핸들러는 CDW10에서 key, CDW12에서 NLB, CDW13에서 value 크기를 추출해 신규 모듈 `kv_cmd.c`의 `kv_put()`/`kv_get()`을 호출한다. Project #1에서 custom command를 등록·디스패치했던 것과 동일한 구조다. 신규 파일 `kv_cmd.{c,h}`가 추가되었고, `InitFTL()` 마지막에 `InitKV()`로 인덱스를 초기화한다.

### 2.3 Index 자료구조: DRAM 상주 Open-Addressing Hash Table

#### 구조 (그림으로 옮길 것)

```
KV_HASH_TABLE @ DRAM 0x2000_0000 (64MB, 정적 예약)
+-----------------------------------------------------------+
| bucket[0]      | key (4B) | slot (4B) |  ← slot=0xFFFFFFFF = empty
| bucket[1]      |   42     |    17     |  ← key 42의 value는 논리 4KB 블록 #17
|   ...          |          |           |
| bucket[2^23-1] |          |           |   해시: h = key * 2654435761 mod 2^23
+-----------------------------------------------------------+   충돌: linear probing (h+1, h+2, ...)

GET(key) : h부터 선형 탐사 → key 일치 버킷의 slot 반환, empty 버킷 도달 시 "No such key"
PUT(key) : 탐사 중 key 발견 → 기존 slot 재사용(덮어쓰기) / empty 도달 → 새 slot 할당 후 기록
```

- 버킷 2^23개 × 8B = **64MB**, 멀티플리커티브 해싱(Knuth 상수 2654435761)
- 빈 버킷 표식은 `slot == 0xFFFFFFFF` (key 0이 유효하므로 0으로 초기화하면 안 됨 — `InitKV()`에서 전체를 0xFF로 초기화)
- OpenSSD에는 OS/힙이 없으므로 메모리는 정적으로 예약했다. `memory_map.h` 분석 결과 FTL 관리 구조의 끝은 약 `0x1A06_0000`이고, `main.c`의 MMU 설정상 `0x1800_0000–0x3FFF_FFFF`는 cached 영역이다. 인덱스는 CPU만 접근하므로(DMA 무관) cached 영역인 `0x2000_0000`에 배치했고, `InitKV()`에서 영역 충돌을 ASSERT로 확인한다.

#### 선정 이유

1. **워크로드 적합성**: 본 과제의 키는 4B 고정, 연산은 점 조회(PUT/GET)뿐이다. 범위 질의가 없으므로 정렬 구조(B+-tree, LSM)의 장점이 발휘될 여지가 없고, O(1) 점 조회가 가능한 해시가 최적이다.
2. **용량 계산이 닿음**: 평가 최대 keyspace는 4,194,304(2^22)이므로 2^23 버킷이면 최악에도 부하율 ≤ 50% — 선형 탐사의 기대 탐사 길이가 ~1.5회로 짧다. 64MB는 보드 DRAM(1GB)의 6.25%로 여유롭다.
3. **무 OS 환경 단순성**: 포인터 없는 평면 배열이라 동적 할당이 필요 없고, 충돌 처리(선형 탐사)가 캐시 친화적이다.

#### 장단점

| 장점 | 단점 |
|---|---|
| O(1) 점 조회/삽입, 구현 단순 | 범위 질의·정렬 순회 불가 |
| 고정 64MB로 메모리 사용 예측 가능 | 휘발성: 전원이 꺼지면 인덱스 소실 (영속화 미구현 — §5 한계) |
| 부하율 50% 이하에서 탐사 길이 짧음 | 키 수가 버킷 수에 근접하면 성능 급락 (용량 검사로 삽입 거부) |
| 삭제 연산이 없어 tombstone 불필요 | 키 크기 4B 고정 가정 (가변 키는 별도 설계 필요) |

### 2.4 Value 관리 기법

#### 저장 위치 결정 — "slot per key" 논리 주소 매핑

스펙의 힌트대로 **FTL이 제공하는 논리 선형 주소 공간을 펌웨어 내부의 value 저장소로 사용**한다. value 크기가 4KB 고정이므로:

- 키가 처음 PUT되면 단조 증가 카운터 `kvNextFreeSlot`에서 **slot(논리 4KB 블록 번호) 하나를 영구 할당**하고 인덱스에 `key→slot`을 기록한다. (append-only 할당)
- 같은 키의 재-PUT은 **같은 slot을 제자리 덮어쓰기**한다. 논리 주소는 같아도 GreedyFTL의 `AddrTransWrite()`가 매번 새 물리 슬라이스를 할당하고 구 슬라이스를 invalidate하므로, NAND 차원의 out-of-place 쓰기·웨어레벨링·GC는 기존 FTL이 그대로 수행한다. 덕분에 value 로그에 대한 별도 GC를 구현할 필요가 없다 (덮어쓰기로 무효화된 공간 회수가 FTL GC로 일원화됨).

```
   key space                 KV index            FTL 논리 주소 공간 (4KB 블록)         NAND
  ───────────   hash    ┌──────────────┐       ┌────┬────┬────┬────┬───────┐   ┌─────────────┐
   key 42  ───────────▶ │ 42 → slot 17 │ ────▶ │ 0  │ 1  │ ...│ 17 │  ...  │──▶│ FTL 페이지 매핑│
   key 7   ───────────▶ │ 7  → slot 18 │       └────┴────┴────┴────┴───────┘   │ + Greedy GC  │
  (재-PUT은 같은 slot을  └──────────────┘        slot 4개 = 슬라이스(16KB) 1개     └─────────────┘
   덮어씀 → 최신값 보장)                          (부분 쓰기는 기존 RMW 경로가 처리)
```

#### PUT 경로 (최신 value 저장)

```
kv_put(tag, key, size, nlb):
  nlb≠0 또는 인덱스/용량 초과 → set_auto_nvme_cpl(에러 status)로 즉시 완료 (인덱스 오염 없음)
  slot = KvLookupOrInsert(key)
  ReqTransNvmeToSlice(tag, slot, 0, IO_NVM_WRITE)   ← 기존 블록 쓰기 경로 전체를 재사용
```

기존 쓰기 경로를 재사용하므로 (1) 호스트 버퍼→데이터버퍼 RxDMA, (2) 슬라이스(16KB)의 나머지 12KB에 대한 read-modify-write, (3) LRU 축출 시 NAND flush, (4) 하드웨어 auto-completion(DW0=0)이 모두 검증된 코드로 처리된다. 4KB value 4개가 슬라이스 하나를 공유하므로 공간 낭비가 없다.

#### GET 경로 (최신 value 검색) — 동기 처리 + 수동 완료

GET은 CQE DW0에 **value 길이(4096)** 를 실어야 하므로 하드웨어 auto-completion(DW0=0)을 쓸 수 없다. 따라서 `kv_get()`은 명령을 동기적으로 끝까지 처리한다:

```
kv_get(tag, key, ...):
  slot = KvLookup(key)
  없으면 → set_auto_nvme_cpl(tag, 0, 0xF82)        // 호스트 가시 status 0x7C1 = ENOSUCHKEY
  SyncAllLowLevelReqDone()                          // ① 파이프라인 드레인: 선행 PUT의 DMA/NAND 정착
  lsa = slot / 4 슬라이스를 데이터버퍼 해시에서 탐색   // ② 버퍼 히트 → 그 DRAM 사본이 곧 최신값
  미스 → NAND 동기 읽기                              // ③ AddrTransRead로 VSA 변환, NAND read 요청을
        (기존 버퍼-미스 경로와 동일한 필드/순서로 구성    //    데이터버퍼 엔트리에 적재 후 다시 드레인
         + EvictDataBufEntry로 선점 엔트리 flush)
  set_auto_tx_dma(tag, 0, 버퍼주소+slot%4*4096, OFF) // ④ 4KB TxDMA (완료는 수동)
  check_auto_tx_dma_done()
  set_auto_nvme_cpl(tag, 4096, SUCCESS)             // ⑤ DW0=4096으로 수동 완료
```

**최신값(latest-write-wins) 보장 논거**: (a) 같은 키는 항상 같은 slot에 쓰이므로 위치가 유일하다. (b) GET은 처리 전 `SyncAllLowLevelReqDone()`으로 모든 미완료 RxDMA/NAND 쓰기를 정착시키므로, 데이터버퍼에 있으면 그것이 최신이고(쓰기 캐시 일관성), 없으면 NAND의 최신 VSA 매핑이 최신이다. (c) NVMe 완료 순서상 호스트는 PUT 완료 후에만 GET을 발행하며(벤치는 단일 스레드 동기 ioctl), 펌웨어는 PUT의 RxDMA 발행 전에 버퍼를 dirty로 등록하므로 완료가 데이터 적재를 앞설 수 없다.

**No-such-key 시맨틱**: 펌웨어가 statusFieldWord `(0xC1<<1)|(0x7<<9)=0xF82`로 완료하면 NVMe CQE status field는 SCT=0x7(vendor specific), SC=0xC1이 되고, 리눅스 NVMe 드라이버는 ioctl 반환값으로 `0x7C1`을 돌려준다. 이는 호스트 `nvme_passthru.h`의 `ENOSUCHKEY`와 일치한다.

### 2.5 검증

- 호스트 로직 테스트(개발 머신): 인덱스 모듈에 100만 회 무작위 PUT/GET을 가해 참조 구현과 전수 비교 — 889,705개 고유 키 전부 일치, slot 중복 없음, 빈 키 오탐 없음, 거부된 PUT의 인덱스 오염 없음을 확인.
- 보드 검증 절차: `make` 후 `sudo ./kv_bench /dev/nvmeXn1 10000 4096 1` → 통과 시 `sudo ./kv_bench /dev/nvmeXn1 1000000 4194304 1`.

  `[캡쳐] kv_bench 기본 파라미터(10000, 4096) 실행 결과 — result: OK=... FAIL=0 NO-SUCH-KEY=1`

  `[캡쳐] kv_bench (1000000, 4194304) 실행 결과`

## 3. 10,000,000회 PUT (keyspace 4,194,304) 테스트 분석

### 3.1 원본 kv_bench가 실패하는 이유 — 호스트 메모리 제약

원본 `kv_bench`는 검증을 위해 `std::unordered_map<uint32_t, std::string> latest`에 **키마다 4KB value 문자열 전체**를 보관한다. ops=10,000,000, keyspace=4,194,304이면 keyspace의 사실상 전부(쿠폰 수집 기대값상 약 92%)가 한 번 이상 쓰여 고유 키가 약 380~420만 개에 달하고, 호스트 메모리 사용량은

- value 본문: ≈ 4.19M × 4096B ≈ **16 GB**
- `std::string`/해시맵 노드 오버헤드(할당 헤더, 버킷, 패딩): 수 GB 추가

로 일반 평가 머신의 물리 메모리를 초과한다. 그 결과 스와핑/OOM으로 PUT·GET 호출이 실패하거나 프로세스가 강제 종료되어 FAIL이 발생한다. 즉 **병목은 SSD가 아니라 호스트 검증 자료구조의 메모리 풋프린트**다.

### 3.2 수정한 kv_bench (`kv_bench_10m.cc`) — 검색 로직 불변, 메모리만 절감

모든 value는 `pattern_for(key, gen)`이 만드는 단일 바이트 패턴의 4KB 반복이므로 **(key, 마지막 PUT의 세대번호 gen)만 있으면 기대값을 완전 재구성**할 수 있다. 수정본은:

- `latest`를 `unordered_map<uint32_t, std::string>` → `unordered_map<uint32_t, uint64_t>`(세대번호 8B)로 변경
- GET 검증 시점에 `std::string exp(4096, pattern_for(key, gen))`으로 기대값을 재생성하여 기존과 동일하게 **바이트 단위 전수 비교**

`KeyValuePut`/`KeyValueGet` 호출, no-such-key 프로브, 비교(검색) 로직은 한 줄도 바꾸지 않았다. 호스트 메모리는 약 16GB → 수십~수백 MB로 감소한다.

### 3.3 디바이스 측 수용 가능성 분석

- 인덱스: 고유 키 ≤ 4,194,304 < 버킷 2^23의 50% → 탐사 길이 안정
- 용량: 4,194,304 slot × 4KB = 16GB valid 데이터 ≪ 노출 용량 약 57GB(전체 64GB − 예약/OP) → FTL GC가 충분한 여유 공간에서 동작
- 쓰기량: 10M PUT × 슬라이스 flush ≤ 160GB ≈ 보드 NAND 2.5 full-drive write — SLC 내구성 내 무리 없음

`[캡쳐] sudo ./kv_bench_10m /dev/nvmeXn1 10000000 4194304 1 실행 결과]`

(실행 결과 FAIL이 발생할 경우: 발생 지점이 PUT인지 GET인지, dmesg의 NVMe 타임아웃 여부를 첨부하고, §5의 개선 방향 — 인덱스 영속화·GET 파이프라인화 — 과 연관지어 원인을 분석할 것.)

## 4. 구현 변경 요약

| 파일 | 변경 |
|---|---|
| `kv_cmd.h` / `kv_cmd.c` | **신규** — KV 인덱스, kv_put/kv_get, InitKV |
| `nvme/nvme_io_cmd.c` | `#include "../kv_cmd.h"` 추가 (디스패치 스켈레톤은 제공분 사용) |
| `ftl_config.c` | `InitFTL()`에서 `InitKV()` 호출 |
| `request_transform.h` | 기존 함수 `EvictDataBufEntry`/`DataReadFromNand` 프로토타입 공개 |
| `data_buffer.h` | 실제 심볼과 불일치하던 extern 선언 수정 (`dataBufHashTablePtr`) |
| (호스트) `kv_bench_10m.cc`, `Makefile` | 10M 추가 과제용 수정본 |

## 5. 한계 및 개선 방향

1. **인덱스 휘발성**: 해시 테이블이 DRAM에만 있어 전원 차단 시 key→slot 매핑이 소실된다. 개선: 인덱스를 주기적으로(또는 shutdown 핸들러에서) 예약 논리 영역에 체크포인트하고, 부팅 시 복구. 더 나아가 슬라이스 spare 영역에 key를 함께 기록해 스캔 복구를 지원할 수 있다.
2. **GET의 동기 처리**: GET마다 전체 파이프라인을 드레인하므로 다중 큐·깊은 QD 환경에서 처리량이 제한된다. 개선: 요청 포맷에 "수동 완료 + DW0 지정" 옵션을 추가해 GET도 비동기 파이프라인에 태우기.
3. **고정 크기 value/key**: 4KB value, 4B key 가정. 가변 크기를 지원하려면 인덱스 엔트리에 길이를 추가하고, BandSlim[4]처럼 작은 value를 페이지에 패킹하는 할당자가 필요하다.
4. **slot 영구 점유**: DELETE가 없어 문제되지 않지만, 삭제를 지원하려면 free-slot 리스트와 인덱스 tombstone이 필요하다.
5. **PUT 완료 시 DW0**: 하드웨어 auto-completion이 DW0=0을 기록한다는 동작에 의존한다(일반 블록 쓰기와 동일 경로라 위험은 낮음). 문제가 관찰되면 GET처럼 RxDMA 후 `set_auto_nvme_cpl(tag, 0, SUCCESS)`로 수동 완료하는 폴백을 적용한다.

## 6. 참고 문헌

[1] Y. Jin, H.-W. Tseng, Y. Papakonstantinou, S. Swanson, "KAML: A Flexible, High-Performance Key-Value SSD," *IEEE HPCA*, 2017, pp. 373–384.

[2] C.-G. Lee et al., "iLSM-SSD: An Intelligent LSM-tree based Key-Value SSD for Data Analytics," *IEEE MASCOTS*, 2019, pp. 384–395.

[3] C. Park et al., "AnyKey: A Key-Value SSD for All Workload Types," *ACM ASPLOS '25*, 2025, pp. 47–63.

[4] J. Park et al., "BandSlim: A Novel Bandwidth and Space-Efficient KV-SSD with an Escape-from-Block Approach," *ICPP '24*, 2024, pp. 1187–1196.

[5] NVM Express, *NVM Express Base Specification, Revision 1.3* (Status Field, Vendor Specific Status Code Type), 2017.

[6] D. E. Knuth, *The Art of Computer Programming, Vol. 3: Sorting and Searching*, 2nd ed., Addison-Wesley, 1998 (multiplicative hashing, §6.4).

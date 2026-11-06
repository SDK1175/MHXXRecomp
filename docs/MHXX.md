# MHXX Recomp — 진행 상황

MHGU Recomp(`morerokk/MHGURecomp` @ `ebe50a3`)를 포크해 **몬스터헌터 더블크로스 Nintendo Switch Ver. (일본판) 1.5.1**로 옮기는 작업.

> **기본 플레이 가능 (2026-11-06).** 리컴파일(점프 테이블 미검출 0개) → 부팅 → 한글패치(romfs 모드) → 세이브 불러오기 → 베르나 마을 진입까지 확인. 포트 전용 기능은 주소 미이식으로 전부 꺼져 있어 원작 그대로 30fps.
> 새 캐릭터 생성(이름 입력 포함)과 채팅(영문)도 정상. **알려진 문제:** 창의 X 버튼으로 안 꺼짐.

## 대상 빌드

| | 값 |
|---|---|
| 타이틀 ID | `0100C3800049C000` |
| 버전 | 1.5.1 |
| `main` 빌드 ID | `9D4C86E6EF74504A` |
| `sdk` / `subsdk0` / `rtld` | `CB8E7F9A74A4A854` / `C2774FA4CACE9ECC` / `E74A8D1013372A3A` |

전부 `runtime/game/game.h`에 있습니다.

**한글패치:** `MHXX_NX_1.5_kor_v2`의 exefs는 순정 1.5.1 exefs와 바이트 단위로 같습니다(2026-11-06 확인). 한글화는 romfs만 바꾸므로 빌드는 하나로 충분하고, 한글패치는 압축을 그대로 `mods/` 아래에 풀어 넣으면 됩니다. 패치에 들어 있는 exefs 파일은 게임 것과 같아서 무시됩니다(`runtime/loader.c`).

## 끝난 것

- **게임 식별**: 타이틀 ID, 빌드 ID 검사, 표시 버전, 기본 언어(`ja`), 창 제목, 안내 문구
- **MHGU Recomp와 분리** (둘 다 설치해도 안 섞이게)
  - 데이터 폴더 `%APPDATA%\MHXXRecomp`, `~/.local/share/MHXXRecomp`
  - 안드로이드 앱 ID `io.github.mhxxrecomp.app`, 별도 서명 키 이름
  - 멀티: 링크 프로토콜 매직 `MHXL`, 버전 `0x5802`, 매칭 키 솔트 변경, **기본 허브 서버 없음** (MHGU 작자의 서버라서. `[Multiplayer] Server =`에 직접 띄운 서버를 넣으면 인터넷 허브 사용 가능, LAN/`Join =`은 그대로)
  - 업데이터: `make UPDATE_REPO=owner/name`으로 지정한 저장소의 릴리스만 설치. 기본은 빈 값 = 업데이트 안 함
- **주소 한곳으로**: 기능들이 쓰던 GU 주소 73개(공용 7 + 기능별 66)를 `runtime/game/addr.h`로 모음. MHXX 값은 전부 `0`이고 GU 값은 옆에 주석으로 남김.
  - 기능의 주소가 다 채워져야(`<기능>_PORTED`) 그 기능이 켜짐. 그 전엔 로그 한 줄 남기고 꺼짐.
  - 게임 전역 포인터는 `LDG()`로 읽어서, 주소가 0이면 0을 돌려줌(주소 0을 읽거나 훅하는 일 없음).
- **리컴파일 시점 패치** (`tools/gen.py`): GU의 47개는 `GU_PATCHES`로 그대로 보존(원본과 동일 확인), 실제 적용은 `MHXX_PATCHES`(현재 비어 있음).
- **GU 전용 콘텐츠 제거**: GU 치트 3개, GU 셰이더 목록(`extras/pipelines.list`). 보너스 세이브와 Forge 플러그인은 꺼둠.
- **치트**: `cheats/<빌드ID>.txt` 형식 파일 중 다른 빌드용은 안 읽음.

## 다음 단계

1. **리컴파일**: 순정 1.5.1 exefs + romfs로 `make gen GAME=<폴더>` → `make`
   - romfs 파일 수를 세서 `GAME_ROMFS_MIN_FILES`에 넣기 (지금은 0 = 개수 검사 안 함)
   - `nativeNX/system/app_shader/AppShaderPackage.mfx` 경로가 MHXX에도 있는지 확인 (없으면 시작 검사에서 막힘 → `MHGU_ROMFS_ANY=1`로 우회 가능)
2. **첫 실행**: 타이틀 → 마을 → 퀘스트. 꺼진 기능 없이 순정 상태로 돌아가는지 먼저 확인
3. **필수 기능부터 주소 채우기** (`runtime/game/addr.h`)
   - `A_G_SYSTEM` (FPS 설정의 뿌리, `vi.c`) → 고프레임 관련 픽스들
   - `INFLATE` (로딩 가속), `MOUSECAM`, `CAMERA_JITTER` (FSR)
   - `gen.py`의 해상도·화면비 패치 (`GU_PATCHES`의 `0xdbcd34` 등)
4. 편의 기능 (`HPSCALE`, `VILLAGEHUB`, `CHAT`, `EMSYNC` …), 디버그 기능
5. 한글패치 on/off 둘 다 확인, 한국어 UI·오버레이 폰트, 안드로이드

## 이식된 기능 (2026-11-06)

| 기능 | MHXX 주소 | 확인 방법 |
|---|---|---|
| 로딩 가속 (`INFLATE`, zlib 1.2.8 그대로) | inflate_fast `0x1417520`, adler32 `0x1416fac` | 구조체 오프셋·모드 상수가 GU와 같음 (디스어셈블) |
| 마우스 시점 (`MOUSECAM`) | 카메라 스틱 처리 `0x224d7c` | `+0x97c`/`+0x980`을 r1로 넘기는 호출 2곳, 피치 ±0x3f00, `+0x9e8`/`+0x9ec` |
| 휠 아이템바 (`ITEMBAR`) | 핸들러 `0x4f4e98`, `0x478f14`, `0x478b5c`, `0x4850d8` | 액션 9/0xa/0x43/0x44, 슬롯 `+0x3298`, 플래그 `+0x253c` GU와 같음 |
| 컨트롤러 버튼 매핑 | (런타임만) | `[Controller]`, 설정 > 컨트롤 > Controller buttons |
| 창 X 버튼 | (런타임만) | 오버레이가 없거나 3초 안에 X를 두 번 누르면 바로 종료 |
| F1 메뉴 한국어 | (런타임만) | `[Application] Language = Korean` (새 ini의 기본값). 메뉴 문구, 오류, ini 도움말 줄 전부. 한글 글꼴: `fonts/menu.ttf` → Windows 맑은 고딕 → 리눅스·안드로이드 Noto Sans CJK / 나눔고딕 |

## 해결: 캐릭터 생성 이름 입력 (2026-11-06)

문제가 아니었음. 이름은 커마 메뉴의 **첫 칸 "이름"**에서 정하고(메뉴 단계 4 → 0x87b710 → 텍스트 입력 → `InlineKeyboard::Appear`), 이름 없이 "설정 완료"를 누르면 "이름을 입력해주세요." 경고가 뜸. 포트에서 키보드 그림은 안 나오지만 PC 키보드로 입력, Enter 확정 (`runtime/textinput.c`). 채팅도 같은 경로로 정상. 조사용 추적은 `MHGU_KBDTRACE=1`로 켬 (`runtime/hle/kbdtrace.c`).

## 참고

- 바이너리·환경 변수 이름(`mhgurecomp.exe`, `mhgugame.dll`, `MHGU_*`)은 아직 그대로입니다. 바꾸면 빌드·업데이터·안드로이드 전반을 건드려야 해서 나중으로 미룸.
- 설정 메뉴의 FPS 기본값은 60이지만, `A_G_SYSTEM`을 채우기 전까지 게임은 원래대로 30fps로 돕니다.
- `tools/retool.py`의 `MTDTI_CTOR`(역분석 도구용)도 GU 주소입니다.
- 라이선스: 이 저장소의 코드는 원본과 같은 MIT. 게임 코드·데이터·키는 포함하지 않으며, 각자 자기 기기에서 덤프한 파일로 빌드합니다.

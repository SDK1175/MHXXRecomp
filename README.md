<h1 align="center">MHXX Recomp</h1>

<p align="center"><b>몬스터헌터 더블크로스 Nintendo Switch Ver.</b>를 에뮬레이터 없이 PC에서 네이티브로 실행하는 포팅</p>

<p align="center">
  <a href="https://github.com/SDK1175/MHXXRecomp/releases/latest"><img src="https://img.shields.io/github/v/release/SDK1175/MHXXRecomp?style=flat-square&label=release&labelColor=1d1e22&color=c58a2e" alt="최신 릴리스"></a>
  <img src="https://img.shields.io/badge/platform-Windows-c58a2e?style=flat-square&labelColor=1d1e22" alt="Windows">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-c58a2e?style=flat-square&labelColor=1d1e22" alt="MIT"></a>
  <a href="https://github.com/morerokk/MHGURecomp"><img src="https://img.shields.io/badge/based%20on-MHGU%20Recomp-1d1e22?style=flat-square" alt="MHGU Recomp 기반"></a>
</p>

> **게임 파일은 들어 있지 않습니다.** 본인 스위치에서 뽑은 본인 소유의 게임(일본판 + 1.5.1 업데이트)이 필요합니다.
> 이 프로젝트는 뽑는 방법을 안내하지 않으며, 게임 파일, 키, 그 링크를 제공하지 않습니다. 한글패치는 제작자가 배포하는 곳에서
> 따로 받아 주세요.

MHXX Recomp는 [MHGU Recomp](https://github.com/morerokk/MHGURecomp)를 포크해, 게임 코드를 PC용으로 다시 컴파일하고
스위치의 시스템과 그래픽을 PC에서 다시 구현해 **몬스터헌터 더블크로스 Nintendo Switch Ver. (일본판) 1.5.1**을 실행합니다.
한글패치를 그대로 쓸 수 있습니다.

## 주요 기능

| | |
|---|---|
| **런처** | 게임 파일 설치, 시작 전 설정(해상도, 창 모드, 모드), 바탕화면 바로가기, **자동 업데이트**(새 버전이 나오면 패치 노트와 함께 알려 주고 설치), **도구**(세이브 백업·복원, 모드 zip 설치, PC 사양 검사, 캐시 지우기, 문제 보고). 배경과 로고는 본인 게임 파일의 타이틀 화면에서 읽어 옵니다 |
| **게임 파일 설치** | 게임 원본(NSP / XCI), 1.5.1 업데이트(NSP), 본인 기기의 prod.keys, 한글패치(zip)를 고르면 런처가 복호화하고 업데이트를 합쳐 알아서 설치합니다 |
| **한글패치** | `MHXX_NX_1.5_kor_v2`를 모드로 그대로 사용. 설정 메뉴도 한국어 |
| **고해상도 렌더링** | 원하는 해상도와 화면비(울트라와이드 포함), 시야각, AMD FSR 3.1 업스케일, 이방성 필터링 16x |
| **고프레임** | 최대 240fps. 몬스터 움직임, 점프, 동시 입력 등을 고프레임에 맞게 보정 |
| **고해상도 텍스처** | 런처가 본인 게임 파일로 본인 PC에서 AI 업스케일(Real-ESRGAN)해 만듭니다. 장비 4배, 몬스터 2배. 그래픽카드 메모리를 재서 알맞은 설정을 추천합니다 |
| **편의 기능** | 마우스로 시점 조작, 마우스 휠 아이템바, 키보드·컨트롤러 버튼 바꾸기, 로딩 가속, 마을 이벤트 빨리 넘기기, 다른 창을 볼 때 FPS 낮추기, 게임 중 업데이트 알림 |
| **게임 버튼 아이콘** | 게임 안 버튼 표시(안내 문구, HUD, 아이템 바, 로비)를 Xbox / PlayStation / 키보드로. 버튼·키 설정을 그대로 따라가고, 바꾸면 바로 반영 |
| **설정 메뉴 (F1)** | 게임 중 언제든 열리는 설정 메뉴. 테마 4종(더블 크로스 포함). 런처로 / 바탕화면으로 나가기 |
| **모드와 치트** | 모드 폴더(불러오는 순서, 충돌 확인), 치트 파일. **영문판(MHGU) Forge 플러그인**을 고치지 않고 그대로 사용(실험적, [확인 목록](docs/gu-plugins.md)) |
| **멀티플레이** | 라드민·하마치 같은 가상 LAN 또는 주소로 직접 접속, 버전 확인. **음성 채팅**(누르고 말하기 / 항상 켜기, 잡음 제거, 사람별 음량, 말하는 사람의 헌터 이름 표시) |

## 시작하기

1. [Releases](https://github.com/SDK1175/MHXXRecomp/releases)에서 `Windows-MHXXRecomp.<버전>.zip`을 받아 새 폴더에 풉니다(예: `C:\Games\MHXXRecomp`, `D:\Games\MHXXRecomp`). **`Program Files`, 바탕화면, 문서 폴더는 피해 주세요**: 관리자 권한이나 OneDrive 동기화 때문에 업데이트·모드·캐시 저장이 실패할 수 있습니다.
2. `mhxx-launcher.exe`를 실행합니다.
3. **게임 파일 설치** 화면에서 게임 원본, 업데이트, prod.keys, (한글패치)를 고르고 **설치**.
4. **게임 시작**.

자세한 사용법은 **[플레이 매뉴얼](docs/manual.md)** 을 보세요. 조작, 설정, 고해상도 텍스처, 문제 해결이 정리되어 있습니다.

## 필요 사양

| | |
|---|---|
| OS | Windows 10 / 11 64비트 |
| CPU | AVX2를 지원하는 CPU (대략 2014년 이후의 Intel Core / AMD Ryzen). 셀러론·펜티엄 실버 같은 저전력 CPU는 대부분 지원하지 않습니다 |
| 그래픽카드 | Vulkan 1.2 지원, 메모리 1GB 이상. 고해상도 텍스처(장비+몬스터)는 4GB 이상 권장 |
| 게임 | 몬스터헌터 더블크로스 Nintendo Switch Ver. **일본판 + 1.5.1 업데이트** (타이틀 ID `0100C3800049C000`) |

## 업데이트 내역

| 버전 | 이름 | 내용 |
|---|---|---|
| [0.10.4](docs/changelog/0.10.4.md) | | 게임이 튕기면 런처가 문제 보고를 권함, 런처 창 크기, 영문판 플러그인 호환 목록 갱신 |
| [0.10.3](docs/changelog/0.10.3.md) | | DL(이벤트) 퀘스트 한글로 표시, 3DS / 스위치 세이브 가져오기, 깃허브 이슈 한국어 양식 |
| [0.10.2](docs/changelog/0.10.2.md) | | HUD 옆 1픽셀 선 수정, 넓은 화면에서 동반자 창이 움직이던 것 수정, 고해상도 텍스처 생성 중 드라이버 재시작 대비, 문제 보고에 이전 런처 기록 포함, 설명서 0.10 정리 |
| [0.10.1](docs/changelog/0.10.1.md) | | 다른 창을 볼 때 30fps로 낮추기(백그라운드 FPS), 게임 중 업데이트 알림 |
| [0.10.0](docs/changelog/0.10.0.md) | | 음성 채팅(잡음 제거, 헌터 이름 표시), 게임 버튼 아이콘(Xbox / PlayStation / 키보드), 영문판 Forge 플러그인 지원(실험적), 로딩 대기 단축, 21:9 컷신 검은 띠 수정, 그리기 속도 개선, 원본 포팅의 수정 사항(진동, 고해상도 컷신, 고프레임 동시 입력 등) |
| [0.9](docs/changelog/0.9.md) | | 고프레임 보정, 로딩 시간 단축(41초 → 10초), 21:9 화면 수정, 멀티 버전 확인·가상 LAN 방 찾기, 런처·게임이 꺼지지 않던 문제 수정, 7z·폴더 모드 설치 |
| [0.8.2](docs/changelog/0.8.2.md) | | 고해상도 텍스처를 처음 만들 때 런처가 꺼지던 문제 수정 |
| [0.8.1](docs/changelog/0.8.1.md) | | 고해상도 텍스처 설정이 저장되지 않던 문제 수정 |
| [0.8](docs/changelog/0.8.md) | | 런처 자동 업데이트, 도구(세이브 백업·복원, 모드 zip 설치, PC 사양 검사, 캐시 지우기, 문제 보고), 실행 파일 이름 `MHXX Recomp.exe` |
| [0.7](docs/changelog/0.7.md) | 하이레조 헌팅 | 런처에서 고해상도 텍스처 만들기, 그래픽카드에 맞춘 텍스처 설정, 이전 팩 가져오기 |
| [0.6](docs/changelog/0.6.md) | | 런처의 게임 파일 설치 (NSP / XCI + 키 + 한글패치) |
| [0.5](docs/changelog/0.5.md) | | 런처, 나가기 선택, 더블 크로스 테마, 시작 시 흰 창 수정 |
| [0.4](docs/changelog/0.4.md) | | 이방성 필터링, 텍스처 덤프와 교체 |
| [0.3](docs/changelog/0.3.md) | | 해상도, 화면비, 시야각, FSR, 고프레임 보정 |
| [0.1](docs/changelog/0.1.md) · [0.2](docs/changelog/0.2.md) | | 첫 실행, 로딩 가속, 마우스 시점, 휠 아이템바, 컨트롤러 버튼, 한국어 메뉴 |

## 문제가 생기면

- **[구글 폼으로 남기기](https://docs.google.com/forms/d/e/1FAIpQLScC3lvrLqtWjTighlP7S_UbcYKcbeIt0gnN29hMizKGI6MA5Q/viewform)**: 기능 추천, 버그 제보, 불편했던 점. 로그인 없이 익명으로 쓸 수 있습니다.
- GitHub 계정이 있다면 이 저장소의 [Issues](https://github.com/SDK1175/MHXXRecomp/issues)에 런처 **도구 → 문제 보고**로
  만든 zip(로그와 PC 사양이 들어 있음)을 첨부해 올려 주세요.

**원본 MHGU Recomp 저장소에는 MHXX 문제를 올리지 말아 주세요.**

## 크레딧

- **[MHGU Recomp](https://github.com/morerokk/MHGURecomp)** — Rokk(morerokk)와 기여자들. 이 프로젝트의 거의 모든 기반입니다.
  원본 README는 [docs/README.MHGU.md](docs/README.MHGU.md)에 그대로 남겨 두었습니다.
- **LAS Recomp**, Forge, Dear ImGui / cimgui, AMD FidelityFX FSR 3.1, FSR-Vulkan, VulkanMemoryAllocator, FFmpeg, SDL —
  [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES)
- **한글패치** — 패치 제작자분들. 이 프로젝트에는 포함되지 않습니다.
- **Real-ESRGAN** (xinntao) — 고해상도 텍스처를 만들 때 런처가 내려받아 사용합니다.

## 라이선스와 법적 안내

이 저장소의 코드는 원본과 같은 [MIT 라이선스](LICENSE)입니다. 이 라이선스는 게임이나 게임 코드에는 적용되지 않으며, 빌드가
만드는 재컴파일된 게임 코드(`mhgugame.dll`)도 마찬가지입니다.

이 프로젝트는 캡콤, 닌텐도와 관련이 없고 승인받지 않았습니다. 몬스터헌터는 캡콤의 상표입니다. 저장소에는 게임 코드, 데이터,
키가 없으며, 프로그램은 사용자가 제공한 사본에서 그것들을 읽습니다. 고해상도 텍스처도 배포하지 않습니다. 게임을 구매해
개발사를 지원해 주세요.

---

<sub>개발자용: 소스에서 빌드하는 방법은 [docs/BUILD.md](docs/BUILD.md), 개발 기록은 [docs/devlog.md](docs/devlog.md).</sub>

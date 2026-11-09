# 소스에서 빌드하기 (개발자용)

일반 사용자는 [Releases](https://github.com/SDK1175/MHXXRecomp/releases)의 zip을 쓰면 됩니다. 이 문서는 직접 빌드하려는
개발자를 위한 것입니다. 빌드는 본인 게임 파일에서 게임 코드를 다시 컴파일하므로, 뽑아 둔 `exefs`와 `romfs`가 필요합니다
(저장소에는 게임 코드나 데이터가 없습니다).

## 1. 도구 (Windows)

1. [MSYS2](https://www.msys2.org/)를 설치합니다.
2. **MSYS2 UCRT64** 창에서:
   ```bash
   pacman -S --needed make git diffutils \
     mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld mingw-w64-ucrt-x86_64-pkgconf \
     mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-shaderc mingw-w64-ucrt-x86_64-nasm \
     mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader \
     mingw-w64-ucrt-x86_64-vulkan-memory-allocator mingw-w64-ucrt-x86_64-zlib
   ```
3. Windows용 [Python](https://www.python.org/) 3.10 이상을 설치하고, 명령 프롬프트에서 저장소 폴더로 가서:
   ```bat
   py -3 -m venv tools\venv
   tools\venv\Scripts\python.exe -m pip install -r tools\requirements.txt
   ```

## 2. 게임 코드 다시 컴파일과 빌드

게임 파일 폴더(`exefs\`, `romfs\`가 들어 있는 폴더, 1.5.1 업데이트가 합쳐진 것)를 `GAME=`으로 줍니다. UCRT64 창, 저장소 폴더에서:

```bash
make gen GAME=/f/path/to/game      # 게임 코드를 C로 옮김 (generated/)
make -j8 GAME=/f/path/to/game      # build/에 게임("MHXX Recomp.exe", mhgugame.dll), 런처(mhxx-launcher.exe),
                                   # 텍스처 뽑기(mhxx-texextract.exe), DLL, data/ 하드 링크
```

처음 전체 빌드는 시간이 걸리고, 그 뒤로는 바뀐 것만 다시 컴파일합니다. 게임 동영상용 FFmpeg는 첫 빌드 때 소스를 받아 필요한
부분만 빌드합니다.

## 3. 배포용 zip

```bash
make dist VERSION=0.8.1 ANDROID_DIST=
```

`dist\Windows-MHXXRecomp.0.8.1.zip`: 게임, 게임 코드, 런처, 텍스처 뽑기, DLL, `version.txt`, `licenses\`, 빈 `data\` `mods\`
`cheats\` `bonus\`. 게임 파일, 한글패치, 텍스처, 설정, 세이브는 들어가지 않습니다(`tools/dist.py`).

런처가 업데이트를 맡습니다(예전 업데이터 프로그램은 없어짐). zip의 `version.txt`(= `VERSION`)와 이 저장소
(`SDK1175/MHXXRecomp`, `tools/launcher/update.cpp`)의 최신 릴리스를 비교합니다. 릴리스는 태그를 버전과 같게(`0.8`), 정식
릴리스로, zip을 첨부해 올립니다. 릴리스 설명이 런처의 패치 노트로 보입니다. `version.txt`가 없는 빌드(직접 빌드한 것)는
개발 빌드로 보고 업데이트를 확인하지 않습니다.

## 참고

- 원본 MHGU Recomp의 자세한 빌드 설명(리눅스, 안드로이드, 테스트 등): [README.MHGU.md](README.MHGU.md#building-from-source)
- MHXX 포팅 기록과 주소: [MHXX.md](MHXX.md), `runtime/game/addr.h`, `tools/gen.py`

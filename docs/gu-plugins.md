# 영문판(MHGU) Forge 플러그인 호환 목록

영문판 MHGU 1.4.0용 Forge 플러그인(`.nro`)을 이 포팅(덥크 일어판 1.5.1)에서 쓴 결과입니다. 플러그인 파일은 고치지 않고
그대로 `mods\모드이름\romfs\nativeNX\plugins\`에 넣습니다. 포팅이 영문판 주소를 덥크 주소로 바꿔 보여 줍니다.

- 이 목록의 모드는 각 제작자의 것입니다. 이 포팅은 어떤 모드도 배포하지 않습니다. 받는 곳은 [MHGURecomp-mods](https://github.com/Oddity-git/MHGURecomp-mods) 목록을 참고하세요.
- 영문판과 덥크의 게임 내부가 다른 부분을 쓰는 기능은 동작하지 않을 수 있습니다. 안 되는 기능은 플러그인이 스스로 끄거나, 로그(`log.txt`)에 이유가 남습니다.
- 세이브 데이터를 건드리는 플러그인은 **세이브를 백업한 뒤** 써 주세요.
- 써 보신 결과(되는 것 / 안 되는 것)는 깃허브 이슈나 구글 폼으로 알려 주세요. 문제 보고 zip에 로그가 들어갑니다.

| 플러그인 | 제작자 | 상태 | 비고 |
|---|---|---|---|
| Damage Numbers | PurplefinNeptuna | ✅ 동작 | Classic / Wilds 스타일 모두 |
| Better Inputs | Fexty12573 | ✅ 동작 | 아이템 바·보상 상자·교역 상자 연속 입력, 메뉴 닫기 대기 |
| KatLib | Katt404 | ✅ 동작 | 다른 플러그인이 쓰는 라이브러리(폴더 이름 `00_KatLib` 그대로) |
| MapAssist | Katt404 | ✅ 동작 | KatLib 필요 |
| UIScale | | ✅ 동작 (사용자 제보) | HUD·UI 크기 조절 |
| Confirm on X | | ✅ 동작 (사용자 제보) | 메뉴의 결정/취소 버튼 바꾸기 |
| MHGU DPS Meter | | ✅ 동작 (사용자 제보) | |
| Better Skill Info | | ✅ 동작 (사용자 제보) | |
| Key Quest Markers | PurplefinNeptuna | ❌ 안 됨 (사용자 제보) | 퀘스트 목록의 키·긴급 표시 |
| World(like) Sharpness Gauge | PurplefinNeptuna | ❌ 안 됨 (사용자 제보) | 원래 포팅에서도 **KatLib보다 먼저 로드**해야 동작: 런처 → 모드에서 순서를 KatLib 위로 올려 다시 확인 필요 |
| Monster Weakness Icons | Potobc | ❌ 안 됨 | Forge 플러그인이 아닌 **데이터 모드**(아직 지원 안 함) |
| Faster Field Hunter | Ellipse1638 | 🟡 대부분 동작 | 전투 중에만 스태미나 소모 등 동작. **모두 채집**(설정은 켜지지만 효과 없음)과 **아이템 아이콘**(스스로 꺼짐)은 덥크에서 안 됨 |

확인 전: Sprint/Weapon Trigger Split(Katt404, KatLib 필요: 달리기와 무기 조작을 다른 트리거로 나눔, 월드식 조작), FF14-style Damage Text, Armor Fusion(세이브 관련)

데이터 모드(텍스처·arc 파일, 예: Xbox Controller Layout)는 아직 지원하지 않습니다. 영문판과 덥크의 파일 위치가 달라
넣어도 적용되지 않습니다.

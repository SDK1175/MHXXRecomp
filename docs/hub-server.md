# 허브 서버 운영 (라드민 없이 멀티)

이 포팅의 **로컬 플레이**를 인터넷으로 이어 주는 중계 서버입니다. 방 목록을 보여 주고, 플레이어끼리 직접 연결이 안 될
때 데이터를 대신 전달합니다. 캡콤의 온라인 기능(이벤트 배포, 온라인 집회소)은 흉내 내지 않습니다.

- 프로그램: 저장소의 `server/` (Go, 작은 실행 파일 하나, 설정 파일·데이터베이스 없음)
- 포트: **39510, TCP와 UDP 둘 다**
- 플레이어의 IP 주소는 서로에게 보이지 않고, 서버도 기록하지 않습니다(`-v`를 켜도 IP는 남기지 않음).

## 1. 서버 만들기 (오라클 클라우드 무료)

1. 콘솔 → **Compute → Instances → Create instance**
2. 이름: `mhxx-hub`
3. **Image**: Canonical Ubuntu 24.04
4. **Shape**: AMD → **VM.Standard.E2.1.Micro** ("Always Free" 표시가 있는 것)
5. **SSH keys**: Generate a key pair for me → **Save private key** (예: `F:\MHXX_port\server\mhxx-hub.key`)
   - 이 파일이 서버 열쇠입니다. **누구에게도 보내지 마세요.**
6. **Create** → 상태가 Running이 되면 **Public IP address**를 적어 둡니다.

## 2. 포트 열기 (오라클 쪽)

1. 인스턴스 화면 → **Primary VNIC**의 **Subnet** → **Security Lists** → Default Security List
2. **Add Ingress Rules**를 두 번:
   | Source CIDR | IP Protocol | Destination Port Range |
   |---|---|---|
   | `0.0.0.0/0` | TCP | `39510` |
   | `0.0.0.0/0` | UDP | `39510` |

서버 안의 방화벽은 3단계의 스크립트가 엽니다.

## 3. 서버에 설치 (명령 두 줄)

Windows PowerShell에서 (키 파일 위치와 IP는 바꿔서):

```
icacls F:\MHXX_port\server\mhxx-hub.key /inheritance:r /grant:r "$($env:USERNAME):R"
ssh -i F:\MHXX_port\server\mhxx-hub.key ubuntu@<Public IP>
```

처음 접속하면 `yes`를 입력합니다. 서버에 들어가면:

```
curl -fsSL https://raw.githubusercontent.com/SDK1175/MHXXRecomp/mhxx/server/setup-ubuntu.sh | sudo sh
```

마지막에 `done: the hub server listens on port 39510`이 나오면 끝입니다. 같은 명령을 다시 실행하면 최신 코드로
업데이트됩니다.

확인: `systemctl status mhxx-hubserver` (active (running)이면 정상)

## 4. 주소 이름 붙이기 (DuckDNS, 선택이지만 권장)

IP 대신 이름을 쓰면 서버를 옮겨도 게임 설정을 안 바꿔도 됩니다.

1. https://www.duckdns.org 에 로그인(프로젝트 구글 계정) → 원하는 이름 추가 (예: `mhxx-hub`)
2. **current ip**에 서버의 Public IP를 넣고 **update ip**
3. 주소는 `mhxx-hub.duckdns.org`가 됩니다.

오라클 서버의 공인 IP는 서버를 지우지 않는 한 바뀌지 않아서, 자동 갱신은 없어도 됩니다.

## 5. 게임에서 쓰기

- 지금: F1 → 네트워크 → **허브 서버**에 `mhxx-hub.duckdns.org` (또는 IP) 입력 → 저장
  - 같이 하는 사람 모두 같은 서버를 써야 서로의 방이 보입니다.
  - **매칭 키**를 정해서 넣으면 같은 키를 넣은 친구들끼리만 방이 보입니다.
- 나중에: 잘 돌아가면 `runtime/config.h`의 `LDN_DEFAULT_SERVER`에 주소를 넣어 다음 릴리즈부터 기본 서버로 씁니다.

## 운영

- 상태: `systemctl status mhxx-hubserver`, 로그: `journalctl -u mhxx-hubserver -n 50`
- 오라클은 오래 거의 쓰이지 않는 무료 서버를 회수할 수 있습니다. 회수 경고 메일(프로젝트 메일함)을 확인하세요.
- 사용량: 오라클 콘솔의 인스턴스 → **Metrics**에서 CPU와 네트워크 그래프를 봅니다.
- 제한은 `server/README.md`의 옵션(IP당 연결·방 수 등)으로 조절합니다. 기본값으로 충분합니다.

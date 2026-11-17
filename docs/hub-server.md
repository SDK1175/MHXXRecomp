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

## 1-A. iwinv (서울, 월 5,600원) — 지금 공식 서버

- 콘솔 → 컴퓨트 → 가상 서버 → 생성하기: ZONE **KR1-Lite-Z01**(KR1-ZONE은 1GB 상품이 자주 품절), 하드웨어 **vgna_1_n**,
  운영체제 **Ubuntu 26.04**, 네트워크 **Direct IP 방식**, 블록스토리지·NAT GATEWAY·SSH Key·Script 없음
- 트래픽 하루 20GB(월 620GB) 포함. 실제 사용량은 2명이 1시간에 약 14MB였습니다.
- 접속 정보(IP, 계정, 비밀번호)는 메일과 문자로 옵니다. 처음 접속하면 `passwd`로 **비밀번호부터 바꿉니다.**
- Lite 존에는 iwinv 방화벽(ELCAP)이 없어서, 설치할 때 서버 안의 방화벽으로 나머지 포트를 닫습니다:
  ```
  curl -fsSL https://raw.githubusercontent.com/SDK1175/MHXXRecomp/mhxx/server/setup-ubuntu.sh | CLOSE_OTHERS=1 sh
  ```
  (root로 접속했을 때. 다른 계정이면 `| sudo CLOSE_OTHERS=1 sh`)
- SSH(22) 비밀번호 대입 공격 막기: `apt-get install -y fail2ban && systemctl enable --now fail2ban`
  (확인: `fail2ban-client status sshd`. 서버를 연 지 몇 시간 만에도 차단된 IP가 생깁니다.)
- 비밀번호 대신 **열쇠 로그인**(권장): 내 PC PowerShell에서
  ```
  ssh-keygen -t ed25519 -f F:\MHXX_port\server\mhxx-hub-key
  type F:\MHXX_port\server\mhxx-hub-key.pub | ssh root@<IP> "mkdir -p ~/.ssh && chmod 700 ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys"
  ```
  PowerShell 5는 보낼 때 줄 앞에 BOM, 끝에 CR을 붙여서 서버가 열쇠를 못 알아봅니다. 서버에서 지웁니다:
  ```
  sed -i '1s/^\xEF\xBB\xBF//; s/\r$//' /root/.ssh/authorized_keys
  ```
  `ssh -i F:\MHXX_port\server\mhxx-hub-key root@<IP>`로 비밀번호 없이 들어가지는 걸 확인한 **다음에** 비밀번호 로그인을 끕니다:
  ```
  printf 'PasswordAuthentication no\nKbdInteractiveAuthentication no\nPermitRootLogin prohibit-password\n' > /etc/ssh/sshd_config.d/00-keys-only.conf && sshd -t && systemctl reload ssh
  ```
  열쇠 파일을 잃어버리면 iwinv 콘솔의 VNC VIEWER로 (비밀번호로) 들어갑니다.

## 1-B. AWS 무료 플랜으로 만들기 (오라클 대신)

처음 테스트 서버(시드니)를 이 방법으로 만들었습니다.

- AWS **무료 플랜**은 가입할 때 정해진 지역(이 계정은 **시드니**)에서만 쓸 수 있습니다. 서울을 쓰려면 유료 플랜으로 바꿔야 합니다.
- 기간은 6개월 또는 크레딧(100달러 + 활동 보너스)을 다 쓸 때까지입니다. 크레딧이 떨어져도 자동으로 유료로 바뀌지 않고 계정이 멈춥니다. **5개월쯤에는 오라클로 옮기세요.**
- 예상 사용량: 서버 약 10달러 + 공인 IP 약 3.6달러 + 디스크 약 1달러 = **한 달 약 15달러**

1. 콘솔 → **EC2 → 인스턴스 시작**
2. 이름 `mhxx-hub`, OS **Ubuntu Server (LTS, 프리 티어 사용 가능)**, 인스턴스 유형 **t3.micro**
3. **키 페어**: 새로 생성 → RSA, `.pem` → `F:\MHXX_port\server\mhxx-hub.pem` (누구에게도 보내지 마세요)
4. **네트워크 설정 → 편집**:
   | 유형 | 포트 | 소스 |
   |---|---|---|
   | SSH | 22 | **내 IP** |
   | 사용자 지정 TCP | `39510` | 위치 무관 |
   | 사용자 지정 UDP | `39510` | 위치 무관 |
5. **고급 세부 정보**: 종료 방지 **활성화**, 크레딧 사양 **표준** (무제한이면 CPU를 많이 쓸 때 크레딧이 더 나갑니다)
6. **인스턴스 시작** → **퍼블릭 IPv4 주소**를 적어 둡니다. 인스턴스를 *중지*했다가 켜면 이 주소가 바뀌니, 그때는 4단계에서 DuckDNS의 IP만 바꾸면 됩니다.
7. 3단계와 같이 설치합니다 (`ssh -i ...mhxx-hub.pem ubuntu@<IP>`).
8. **예산 경보**: 콘솔 검색 → Budgets → 예산 생성 → 월별 비용 예산 (예: 15달러, 프로젝트 메일). 크레딧 보너스도 받습니다.

## 4. 주소 이름 붙이기 (DuckDNS, 선택이지만 권장)

IP 대신 이름을 쓰면 서버를 옮겨도 게임 설정을 안 바꿔도 됩니다.

1. https://www.duckdns.org 에 로그인(프로젝트 구글 계정) → 원하는 이름 추가 (예: `mhxx-hub`)
2. **current ip**에 서버의 Public IP를 넣고 **update ip**
3. 주소는 `mhxx-hub.duckdns.org`가 됩니다.

오라클 서버의 공인 IP는 서버를 지우지 않는 한 바뀌지 않아서, 자동 갱신은 없어도 됩니다.

## 5. 게임에서 쓰기

- 지금: F1 → 네트워크 → **허브 서버**에 `mhxx-kr.duckdns.org` (또는 IP) 입력 → 저장 → **허브 서버 확인**에 "온라인, ○○ ms"가 나오면 연결된 것
  - 같이 하는 사람 모두 같은 서버를 써야 서로의 방이 보입니다.
  - **매칭 키**를 정해서 넣으면 같은 키를 넣은 친구들끼리만 방이 보입니다.
- 나중에: 잘 돌아가면 `runtime/config.h`의 `LDN_DEFAULT_SERVER`에 주소를 넣어 다음 릴리즈부터 기본 서버로 씁니다.

## 운영

- 상태: `systemctl status mhxx-hubserver`, 로그: `journalctl -u mhxx-hubserver -n 50`
- 오라클은 오래 거의 쓰이지 않는 무료 서버를 회수할 수 있습니다. 회수 경고 메일(프로젝트 메일함)을 확인하세요.
- 사용량: 오라클 콘솔의 인스턴스 → **Metrics**에서 CPU와 네트워크 그래프를 봅니다.
- 제한은 `server/README.md`의 옵션(IP당 연결·방 수 등)으로 조절합니다. 기본값으로 충분합니다.

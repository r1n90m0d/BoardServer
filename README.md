# BoardServer

SmarcleSafe 보안 CCTV 시스템의 **웹캠 보드 쪽 코드와 운영 문서**를 모아 둔 저장소입니다.
보드(LuckFox Pico Zero)에서 돌아가는 상태 보고 데몬(heartbeat), 보드 설정을 한 번에 적용하는 배포 스크립트, 보드 운영 중 알게 된 내용을 정리한 문서가 들어 있습니다.

## 시스템에서 이 저장소의 위치

```
 웹캠 보드 (이 저장소)                 클라우드 서버 (별도 저장소)          클라이언트
 ┌────────────────────────┐           ┌──────────────────────────┐      ┌───────────┐
 │ heartbeat ── HTTP POST ─┼── VPN ──▶ │ controller :3000          │      │ 웹 대시보드 │
 │ rkipc :554 ◀─ RTSP pull ┼── VPN ─── │ mediamtx ── WebRTC (WHEP) ┼─VPN─▶│ 안드로이드 앱│
 └────────────────────────┘           └──────────────────────────┘      └───────────┘
            └──────────────── WireGuard (wg1) ────────────────┘
```

- 보드와 서버 사이 통신은 모두 WireGuard VPN 터널(`wg1`) 안에서 이뤄집니다. 서버의 VPN 주소는 `10.0.0.100`입니다.
- **heartbeat**: 보드가 5초마다 서버에 자기 상태를 보고합니다. 서버는 이것으로 보드의 online/offline을 판단합니다.
- **영상**: 보드가 서버로 밀어 보내지 않습니다. 서버의 mediamtx가 보드 rkipc의 RTSP(554/tcp)에 접속해서 가져가고(pull), 클라이언트에는 WebRTC로 내보냅니다.
- 서버(Node.js/Express, AWS)와 클라이언트 코드는 이 저장소에 없습니다.

## 하드웨어 / 개발 환경

| 항목 | 내용 |
| --- | --- |
| 보드 | LuckFox Pico Zero (Rockchip RV1106G3), eMMC 부팅 |
| 카메라 센서 | Sony IMX415 |
| 보드 OS | LuckFox SDK의 Buildroot 리눅스 (커널 5.10, uClibc) |
| 개발 PC | Windows 위 VMware Ubuntu 게스트. 보드는 USB로 연결하고 `adb`로 접근 |
| SDK | [LuckFox Pico SDK](https://github.com/LuckfoxTECH/luckfox-pico)를 `~/luckfox-pico`에 받아 둔 상태를 전제로 함 |

## 디렉토리 구조

```
BoardServer/
├── heartbeat/
│   ├── heartbeat.c          상태 보고 데몬 소스 (C, libcurl)
│   ├── Makefile             크로스 컴파일 설정
│   ├── S60heartbeat         보드 부팅 시 heartbeat를 띄우는 init 스크립트
│   └── tools/mock_server.py heartbeat 수동 테스트용 모의 서버
├── deploy/
│   └── install_board.sh     보드 1대에 필요한 설정과 heartbeat를 adb로 설치
├── docs/
│   ├── heartbeat-install.md     install_board.sh 사용법, 확인·되돌리기 방법
│   ├── rkipc-clock-fix.md       보드 시계가 8시간 앞서는 문제의 원인과 수정 방법
│   ├── dev-environment-guide.md 개발 환경(SDK·저장소·보드) 정보를 정리하는 기준
│   ├── board-snapshot.md        adb로 조회한 보드 상태 기록 (최신이 위)
│   └── progress/I2.md           현재 iteration(I2) 체크리스트와 작업 로그
└── CLAUDE.md                AI 코딩 도구(Claude Code)용 프로젝트 지침
```

## heartbeat

보드에서 데몬으로 실행되며, 5초마다 서버에 아래 JSON을 `POST`합니다.

```json
{ "mac": "<wlan0 MAC, 대문자·콜론 없음>", "status": "online", "rtsp_ok": true, "wg_ok": true, "config_version": 0 }
```

| 필드 | 판단 방법 |
| --- | --- |
| `mac` | `/sys/class/net/wlan0/address`를 읽어 콜론을 빼고 대문자로 바꿈 |
| `rtsp_ok` | `rkipc` 프로세스가 실행 중인지 (영상이 정상인지까지는 보지 않음) |
| `wg_ok` | `wg1` 인터페이스가 UP인지 (핸드셰이크 성공 여부는 보지 않음) |

- 전송 실패, 타임아웃(연결 2초, 전체 3초), 403 같은 오류가 나도 종료하지 않고 다음 주기에 다시 보냅니다.
- 로그는 상태나 결과가 바뀔 때만 남기고, 보드의 syslog(`/var/log/messages`, 태그 `heartbeat`)로 보냅니다.
- `SIGTERM`/`SIGINT`를 받으면 정리하고 종료합니다.

**환경 변수**

| 변수 | 기본값 | 설명 |
| --- | --- | --- |
| `HEARTBEAT_URL` | `http://10.0.0.100:3000/v1/devices/heartbeat` | 보낼 주소. 보드에서는 `S60heartbeat`가 지정함 |
| `HEARTBEAT_IFACE` | `wg1` | 요청을 이 인터페이스로만 내보냄. 빈 문자열이면 고정하지 않음(로컬 테스트용) |

### 빌드

보드에서 직접 빌드하지 않고, 개발 PC에서 SDK 툴체인으로 크로스 컴파일합니다.

```sh
cd heartbeat
make            # 결과물: heartbeat/heartbeat (ARM 32bit, uClibc)
make clean
```

툴체인(`arm-rockchip830-linux-uclibcgnueabihf-gcc`)과 sysroot 경로는 `~/luckfox-pico` 기준으로 Makefile에 들어 있습니다. SDK를 다른 곳에 두었다면 `make CC=... SYSROOT=...`로 바꿔 줍니다. SDK의 Buildroot를 한 번 빌드해 sysroot에 libcurl이 있어야 링크됩니다.

### 모의 서버로 테스트

실서버 없이 동작을 확인하려면 개발 PC에서 모의 서버를 띄우고, 보드의 heartbeat가 그쪽으로 보내게 합니다.

```sh
# 개발 PC
python3 heartbeat/tools/mock_server.py --mode 200          # 항상 200 응답
python3 heartbeat/tools/mock_server.py --mode 403          # 항상 403 응답
python3 heartbeat/tools/mock_server.py --mode noresponse --hold-seconds 10   # 응답 없이 대기

# 보드 (같은 Wi-Fi에서, wg1 고정을 풀고 실행)
HEARTBEAT_URL=http://<개발 PC IP>:3000/v1/devices/heartbeat HEARTBEAT_IFACE= /usr/bin/heartbeat
```

모의 서버는 요청 본문과 이전 요청과의 간격을 출력하므로, 주기와 재시도 동작을 눈으로 확인할 수 있습니다.

## 보드에 배포

`deploy/install_board.sh`는 adb로 연결된 보드 1대에 다음을 적용합니다. 자세한 내용은 [docs/heartbeat-install.md](docs/heartbeat-install.md)를 참고하세요.

1. rkipc의 8시간 시계 오차 수정 (원인: [docs/rkipc-clock-fix.md](docs/rkipc-clock-fix.md))
2. dhcpcd가 `wg*` 인터페이스를 건드리지 않게 설정
3. `/usr/bin/heartbeat`, `/etc/init.d/S60heartbeat` 설치

```sh
cd heartbeat && make && cd ..
./deploy/install_board.sh --dry-run    # 무엇이 바뀔지 먼저 확인
./deploy/install_board.sh              # 실제 적용 (재부팅은 직접: adb reboot)
```

- 여러 번 실행해도 결과가 같습니다. 이미 적용된 항목은 건너뜁니다.
- 수정하는 파일은 처음 한 번만 `*.bak-install`로 백업합니다.
- 보드가 여러 대 연결돼 있으면 `--serial <adb 시리얼>`을 붙입니다.
- **WireGuard 설정(`wg1.conf`)과 키는 다루지 않습니다.** 보드마다 따로 설정해야 합니다.

## 진행 상황

개발은 iteration 단위로 진행합니다.

- **I1 (완료)**: WireGuard 자동 연결, heartbeat 전송, 웹에서 보드 online/offline 표시
- **I2 (진행 중)**: 웹·앱에서 보드 영상 재생. 보드 쪽 작업은 rkipc 스트림 설정(고화질 1080p H.265, 저화질 540p H.265)과 서버가 VPN으로 영상을 가져가는지 확인하는 것입니다. 체크리스트는 [docs/progress/I2.md](docs/progress/I2.md)에 있습니다.

I2 요구사항·인터페이스(ICD) 원문은 팀 노션에서 관리하며, 서버 주소와 보드 식별 정보가 들어 있어 이 공개 저장소에는 올리지 않습니다.

## 보안 관련 안내

이 저장소는 공개 저장소입니다. WireGuard 키, 서버 공인 IP, 보드 MAC, 계정 정보는 커밋하지 않습니다. 보드 상태 기록(`docs/board-snapshot.md`)에서도 이런 값은 `<redacted>`로 가려 둡니다. 개인 환경 정보는 `CLAUDE.local.md`(git 추적 제외)에 적습니다.

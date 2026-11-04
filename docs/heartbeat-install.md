# heartbeat 설치 가이드 (보드 재플래싱 후 복구용)

보드를 재플래싱(이미지 재굽기)하거나 새 보드에 똑같은 구성을 적용할 때 쓰는 가이드. 실제 적용은 `deploy/install_board.sh` 스크립트가 하고, 이 문서는 그 사용법과 확인 방법을 정리한 것.

## 스크립트가 적용하는 것

1. **rkipc 8시간 시계 오차 수정** — `[network.ntp] enable=0`을 factory ini(실제 경로는 보드/센서에 따라 다름, `/tmp/rkipc-factory-config.ini`가 가리키는 파일)와 `/userdata/rkipc.ini` 양쪽에 적용. 원인 설명은 [`rkipc-clock-fix.md`](rkipc-clock-fix.md) 참고.
2. **dhcpcd가 wg\* 인터페이스를 안 건드리게** — `/etc/dhcpcd.conf`에 `denyinterfaces wg*` 추가.
3. **heartbeat 설치** — `/usr/bin/heartbeat`, `/etc/init.d/S60heartbeat` 설치 + 실행 권한 설정.

몇 번을 실행해도 결과가 같다(이미 적용된 항목은 건너뜀). **WireGuard 설정(`wg1.conf`)이나 개인키는 이 스크립트가 절대 읽거나 건드리지 않는다.**

## 사전 준비

- VM(또는 adb가 설치된 PC)에서 실행.
- `~/BoardServer` 저장소를 clone/pull 받아둘 것.
- `heartbeat/` 폴더에서 **미리 빌드**해 둘 것 (크로스 컴파일, 보드에서 바로 빌드하지 않음):
  ```sh
  cd heartbeat
  make
  ```
  (`CC`/`SYSROOT`는 Makefile에 기본값이 들어있음. 다른 경로를 쓰려면 `make CC=... SYSROOT=...`)
- 보드가 USB로 연결돼 있고 `adb devices`에 표시될 것.

## 사용법

```sh
cd ~/BoardServer
./deploy/install_board.sh [--dry-run] [--serial <ANDROID_SERIAL>]
```

| 옵션 | 설명 |
|---|---|
| `--dry-run` | 아무것도 바꾸지 않고, 뭘 바꿀지만 출력한다. 처음엔 항상 이걸로 먼저 확인할 것. |
| `--serial <id>` | 보드가 여러 대 연결돼 있을 때 지정. `adb devices`로 확인한 시리얼을 넣는다. 이미 쉘에 `export ANDROID_SERIAL=...`가 설정돼 있으면 생략 가능 — 스크립트는 기존 `ANDROID_SERIAL` 값을 그대로 존중한다. |

예시(보드가 1대만 연결된 경우):
```sh
./deploy/install_board.sh --dry-run     # 1) 먼저 뭐가 바뀔지 확인
./deploy/install_board.sh               # 2) 문제없으면 실제 적용
```

여러 대 연결된 경우:
```sh
adb devices                              # 시리얼 확인
./deploy/install_board.sh --dry-run --serial 63d0e45945a2af66
./deploy/install_board.sh --serial 63d0e45945a2af66
```

## 스크립트가 하는 일 (순서대로)

1. **사전 확인** (읽기 전용) — adb 연결, 로컬 heartbeat 바이너리/S60heartbeat 존재, `wlan0` MAC, libcurl 런타임, factory ini 실제 경로, `/oem`·`/userdata`·`/etc` 쓰기 가능 여부. **하나라도 실패하면 아무것도 안 바꾸고 중단한다.**
2. **백업** — 실제로 수정하는 파일만, 그것도 **처음 한 번만** `<원본경로>.bak-install`로 백업. 이미 백업이 있으면 또 만들지 않음(기존 백업을 덮어쓰지 않음).
3. **시계 수정** — factory ini와 `/userdata/rkipc.ini`의 `enable` 값이 둘 다 이미 0이면 건너뛴다(이때는 rkipc를 전혀 건드리지 않음). 하나라도 1이면: rkipc를 SIGTERM으로 정지(5초 뒤에도 살아있으면 SIGKILL) → 백업 → 두 파일의 `enable`을 0으로 수정. rkipc는 다시 띄우지 않음(재부팅 시 새 설정으로 자동 기동됨).
4. **dhcpcd 설정** — `denyinterfaces wg*`가 이미 있으면 건너뛰고, 없으면 파일 끝에 한 줄 추가.
5. **heartbeat 설치** — 로컬 빌드물과 보드에 있는 파일의 md5가 같으면 건너뛰고, 다르면 기존 파일을 백업한 뒤 교체하고 `755` 권한을 설정.
6. **마무리** — `sync` 실행. **재부팅은 하지 않는다** — 뭔가 바뀌었으면 `REBOOT_REQUIRED: 예`를 출력만 하고, 실제 재부팅은 사용자가 직접 `adb reboot`로 하면 된다.

## 적용 후 확인 (수동)

```sh
adb shell "grep -A3 '\[network.ntp\]' /userdata/rkipc.ini"   # enable=0 인지
adb shell "grep denyinterfaces /etc/dhcpcd.conf"              # wg* 있는지
adb shell "pidof heartbeat rkipc ntpd"                        # 재부팅 후 셋 다 떠 있는지
adb shell date -u                                             # 호스트 PC와 시각 비교 (8시간 안 벌어지는지)
```

## 되돌리기(백업에서 복구)

스크립트가 만든 `*.bak-install` 백업으로 되돌리는 예시:
```sh
adb shell "cp /userdata/rkipc.ini.bak-install /userdata/rkipc.ini"
adb shell "cp /etc/dhcpcd.conf.bak-install /etc/dhcpcd.conf"
# factory ini 경로는 보드마다 다를 수 있음 - 먼저 readlink -f /tmp/rkipc-factory-config.ini 로 확인
adb shell "cp <factory-ini-path>.bak-install <factory-ini-path>"
adb shell sync
adb reboot
```

## 미검증 / 알아둘 점

- 이 스크립트는 **"이미 적용된 보드"에서 재실행했을 때 아무것도 안 바뀌는지**는 실제로 확인했지만, **"방금 재플래싱한 완전히 새 보드"에서 처음부터 적용하는 경로는 실제 재플래싱 보드로 테스트하지 못했다** — 기존에 이미 설정이 적용된 보드를 일부러 되돌려서 테스트하면 그 보드 자체가 불안정해질 수 있어 시도하지 않았음.
- `/tmp/rkipc-factory-config.ini` 심링크는 보드가 최소 1번 정상 부팅해야 생긴다 — 재플래싱 직후 아직 부팅이 안 끝난 상태에서 실행하면 사전 확인 단계에서 중단된다(의도된 동작).
- WireGuard(`wg1`) 설정은 이 스크립트 범위에 없음 — 별도로 수동 적용해야 함.

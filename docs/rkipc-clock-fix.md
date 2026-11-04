# rkipc 8시간 시계 오차 수정 가이드

## 증상

보드의 시스템 시계가 실제 UTC보다 정확히 **8시간(28800초) 앞서는** 상태가 반복됨. `date -u`로 확인 가능. 부팅 초반(수십~백여 초)에는 시스템 ntpd가 정확히 보정하지만, 이후 다시 +8시간으로 튀고 그 상태가 유지됨.

## 근본 원인 (사실, 소스 코드와 실제 동작으로 확인됨)

1. **직접 원인**: `~/luckfox-pico/project/app/rkipc/rkipc/common/network/ntp.c`의 `rkipc_ntp_update()` 함수(약 210번째 줄)가 NTP 서버에서 받아온 정확한 UTC 오프셋에 **하드코딩된 `+28800`(8시간, 중국 표준시 UTC+8 가정)**을 더한 뒤 `busybox date -s`로 시스템 시계를 직접 덮어씀. 이 함수는 `/userdata/rkipc.ini`의 `[network.ntp]` 섹션에 `enable=1`일 때 `refresh_time_s`(기본 60초)마다 반복 호출됨(`common/network/network.c` 약 821~827번째 줄).
2. **왜 `enable=0`으로 고쳐도 재부팅하면 되돌아가는가**: `/oem/usr/bin/RkLunch.sh`의 `post_chk()` 함수 안에 다음 코드가 있음(121~123번째 줄 근처):
   ```sh
   #if [ ! -f "$rkipc_ini" ]; then
       cp $default_rkipc_ini $rkipc_ini -f
   #fi
   ```
   가드 조건(`if`/`fi`)이 **주석 처리돼 있어서** `cp $default_rkipc_ini $rkipc_ini -f`가 **매 부팅마다 무조건** 실행됨. `$default_rkipc_ini`는 `/tmp/rkipc-factory-config.ini`(심링크, 센서에 따라 `/oem/usr/share/rkipc-*.ini`를 가리킴 — imx415 센서 보드는 `/oem/usr/share/rkipc-800w.ini`)이고 `$rkipc_ini`는 `/userdata/rkipc.ini`. 즉 **부팅할 때마다 `/userdata/rkipc.ini`가 `/oem/usr/share/rkipc-800w.ini`(또는 해당 센서의 ini)로 통째로 덮어써짐** — `/userdata/rkipc.ini`만 고쳐봐야 다음 재부팅에서 소용없는 이유.
3. (부수적 요인) rkipc는 정상 종료(`SIGTERM`)될 때도 메모리에 있던 설정을 `/userdata/rkipc.ini`에 다시 씀(`rk_param_deinit()` → `rk_param_save()`, `common/param/param.c`). 이 때문에 "rkipc를 안 끄고 파일만 고치면" 종료 시 그 수정이 덮어써질 수도 있음 — 하지만 **핵심 원인은 2번(RkLunch.sh의 무조건 cp)**이며, 2번을 고치지 않으면 1번을 아무리 고쳐도 소용없음.

## 수정 방법

핵심: **"기준이 되는 원본 파일"** (부팅 때마다 `/userdata/rkipc.ini`로 복사되는 소스, 보통 `/oem/usr/share/rkipc-800w.ini` — 보드의 센서에 따라 파일명이 다를 수 있으니 1단계에서 직접 확인할 것)의 `[network.ntp]` `enable` 값을 `0`으로 바꿔야 함. `/userdata/rkipc.ini`만 고치는 것은 임시방편일 뿐, 재부팅하면 다시 덮어써짐.

### 0. 사전 확인 (읽기 전용)

```sh
# 지금 쓰고 있는 센서/원본 ini 확인
readlink -f /tmp/rkipc-factory-config.ini
# → 예: /oem/usr/share/rkipc-800w.ini

# RkLunch.sh의 가드가 정말 주석 처리돼 있는지 확인
grep -n 'rkipc_ini\|cp \$default_rkipc_ini\|^\s*#if\|^\s*#fi' /oem/usr/bin/RkLunch.sh

# /oem이 쓰기 가능한지 확인
mount | grep ' /oem '   # rw 인지 확인
touch /oem/.write_test && echo WRITABLE && rm -f /oem/.write_test
```
`/oem`이 `ro`로 마운트돼 있거나 쓰기 테스트가 실패하면, **이 문서의 방법을 그대로 적용하면 안 됨** — 재마운트 시도하지 말고 원인과 최소 수정안을 먼저 정리할 것.

### 1. rkipc 정지 (메모리 상태가 파일을 덮어쓰지 않게)

```sh
pidof rkipc
kill -TERM <pid>
sleep 5
pidof rkipc   # 비어 있어야 함. 안 죽으면: kill -KILL <pid>
sleep 5
pidof rkipc   # 자동으로 다시 떠 있으면 즉시 멈추고, 무엇이 재시작시키는지 먼저 확인할 것
```

### 2. 백업

```sh
cp /oem/usr/share/rkipc-800w.ini /userdata/rkipc-800w.ini.bak   # 파일명은 0단계에서 확인한 실제 원본으로
```
(선택) `/userdata/rkipc.ini`도 같이 백업해두면 안전: `cp /userdata/rkipc.ini /userdata/rkipc.ini.bak`

### 3. 원본 ini 수정 (핵심)

```sh
sed -n '/\[network.ntp\]/,+3p' /oem/usr/share/rkipc-800w.ini   # enable 줄 번호 확인
sed -i '<줄번호>s/^enable = 1$/enable = 0/' /oem/usr/share/rkipc-800w.ini
diff /userdata/rkipc-800w.ini.bak /oem/usr/share/rkipc-800w.ini   # enable 한 줄만 바뀌었는지 확인
```

### 4. `/userdata/rkipc.ini`도 같이 수정 (당장 반영되게)

```sh
sed -n '/\[network.ntp\]/,+3p' /userdata/rkipc.ini   # 포맷이 다를 수 있음(들여쓰기/패딩), 정확히 확인
sed -i '<줄번호>s/^enable\(\s*\)= 1$/enable\1= 0/' /userdata/rkipc.ini
diff /userdata/rkipc.ini.bak /userdata/rkipc.ini   # (백업했다면) enable 한 줄만 바뀌었는지 확인
```

### 5. sync + 재부팅

```sh
sync
reboot
```

## 확인 방법 (재부팅 후)

```sh
# 1) 두 파일 모두 enable=0 유지되는지
grep -A3 '\[network.ntp\]' /oem/usr/share/rkipc-800w.ini
grep -A3 '\[network.ntp\]' /userdata/rkipc.ini

# 2) 시계가 정확한지 (호스트 PC와 동시에 date -u 비교, 몇 분간 반복 확인 권장)
date -u

# 3) 핵심 프로세스들 정상 실행 중인지
pidof ntpd rkipc heartbeat
```
재부팅 직후 ~3분간 30초 간격으로 `date -u`를 반복 비교해서 8시간 점프가 재발하지 않는지 확인하는 것을 권장함. **rkipc를 미리 정지하지 않은 채로** 재부팅해도(평소처럼 그냥 재부팅) 결과가 같은지 한 번 더 확인하면 더 확실함.

## 백업에서 복구하는 방법 (문제가 생겼을 때)

```sh
pidof rkipc && kill -TERM $(pidof rkipc)   # 켜져 있으면 먼저 정지
cp /userdata/rkipc-800w.ini.bak /oem/usr/share/rkipc-800w.ini
cp /userdata/rkipc.ini.bak /userdata/rkipc.ini   # 백업해뒀다면
sync
reboot
```

## 주의

- `/usr/share/rkipc.ini`(확장자만 있는 경로)는 건드리지 않음 — 센서별 파일(`rkipc-800w.ini` 등)이 실제로 쓰이는 원본임.
- 시스템 NTP(`/etc/ntp.conf`, `S49ntp`) 설정은 건드리지 않음 — 이번 문제와 무관(시스템 ntpd는 정상적으로 올바른 UTC로 보정하고 있었음).
- WireGuard 설정은 이 작업과 무관하므로 읽거나 바꾸지 않음.
- SDK 원본(`~/luckfox-pico`)이나 이미지 빌드는 바꾸지 않음 — 이 문서는 **보드 위에서 실행 중인 파일**(`/oem`, `/userdata`)만 수정하는 절차임.

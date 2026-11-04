#!/usr/bin/env bash
#
# deploy/install_board.sh
#
# VM에서 실행해서 adb로 연결된 보드 1대에 다음을 적용한다:
#   1. rkipc의 8시간 시계 오차 수정 (factory ini + /userdata/rkipc.ini의
#      [network.ntp] enable=0) - 원인은 docs/rkipc-clock-fix.md 참고
#   2. dhcpcd가 wg* 인터페이스를 건드리지 않게 설정
#   3. /usr/bin/heartbeat, /etc/init.d/S60heartbeat 설치
#
# 여러 번 실행해도 결과가 같다(idempotent) - 이미 적용된 항목은 건너뛴다.
# wg1.conf와 WireGuard 개인키는 이 스크립트가 읽거나 건드리지 않는다.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

LOCAL_HEARTBEAT_BIN="$REPO_ROOT/heartbeat/heartbeat"
LOCAL_S60_SCRIPT="$REPO_ROOT/heartbeat/S60heartbeat"

DRY_RUN=0
CHANGED=0

usage() {
	cat <<'EOF'
사용법: deploy/install_board.sh [--dry-run] [--serial <ANDROID_SERIAL>]

VM에서 실행하며, adb로 연결된 보드 1대에 heartbeat 설치와
rkipc 시계 오차 수정, dhcpcd 설정을 적용한다.

옵션:
  --dry-run        아무것도 바꾸지 않고 무엇을 바꿀지만 출력한다.
  --serial <id>    ANDROID_SERIAL 환경변수를 지정한다(여러 대 연결 시 필수).
                   이미 환경변수 ANDROID_SERIAL이 설정돼 있으면 생략 가능.
  -h, --help       이 도움말 출력.

여러 번 실행해도 안전하다 - 이미 적용된 항목은 건너뛴다.
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	--dry-run)
		DRY_RUN=1
		shift
		;;
	--serial)
		[ $# -ge 2 ] || { echo "--serial에는 값이 필요함" >&2; exit 1; }
		ANDROID_SERIAL="$2"
		export ANDROID_SERIAL
		shift 2
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "알 수 없는 옵션: $1" >&2
		usage >&2
		exit 1
		;;
	esac
done

log() { printf '[install_board] %s\n' "$*"; }
warn() { printf '[install_board][WARN] %s\n' "$*" >&2; }
die() {
	printf '[install_board][FATAL] %s\n' "$*" >&2
	exit 1
}
dry() { [ "$DRY_RUN" = "1" ]; }

board() { adb shell "$@"; }

ADB_TARGET_DESC="${ANDROID_SERIAL:-<ANDROID_SERIAL 미설정: 연결된 기본 장치 사용>}"
log "대상 장치: $ADB_TARGET_DESC"
dry && log "(dry-run 모드: 보드 상태를 바꾸는 명령은 실행하지 않습니다)"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

###############################################################################
# 1. 사전 확인 - 하나라도 실패하면 아무것도 바꾸지 않고 중단한다.
###############################################################################
log "=== 1. 사전 확인 ==="

if ! adb get-state >/dev/null 2>&1; then
	die "adb 장치 연결 안 됨 - adb devices로 확인할 것"
fi
log "adb 연결: OK ($(adb get-state 2>/dev/null))"

[ -f "$LOCAL_HEARTBEAT_BIN" ] || die "로컬 heartbeat 바이너리 없음: $LOCAL_HEARTBEAT_BIN (heartbeat/ 에서 make 먼저 할 것)"
log "로컬 heartbeat 바이너리: OK ($LOCAL_HEARTBEAT_BIN)"

[ -f "$LOCAL_S60_SCRIPT" ] || die "로컬 S60heartbeat 스크립트 없음: $LOCAL_S60_SCRIPT"
log "로컬 S60heartbeat 스크립트: OK ($LOCAL_S60_SCRIPT)"

WLAN0_MAC="$(board cat /sys/class/net/wlan0/address 2>/dev/null | tr -d '\r\n')"
[ -n "$WLAN0_MAC" ] || die "보드에서 wlan0 MAC을 못 읽음"
log "wlan0 MAC: $WLAN0_MAC"

LIBCURL_VER="$(board 'curl --version 2>/dev/null | head -1' 2>/dev/null | tr -d '\r')"
[ -n "$LIBCURL_VER" ] || die "보드에 curl/libcurl 런타임이 없음 - heartbeat가 실행될 수 없음"
log "libcurl 런타임: $LIBCURL_VER"

FACTORY_INI="$(board readlink -f /tmp/rkipc-factory-config.ini 2>/dev/null | tr -d '\r\n')"
[ -n "$FACTORY_INI" ] || die "/tmp/rkipc-factory-config.ini 심링크를 못 찾음 (보드가 정상 부팅된 적이 없을 수 있음)"
log "factory ini 실제 대상: $FACTORY_INI"

check_writable() {
	local path="$1"
	if board "touch '$path/.install_write_test' 2>/dev/null && rm -f '$path/.install_write_test'" >/dev/null 2>&1; then
		log "$path 쓰기 가능: OK"
	else
		die "$path 쓰기 불가 - 중단"
	fi
}
check_writable /oem
check_writable /userdata
check_writable /etc

###############################################################################
# 백업 / 파일 유틸
###############################################################################
backup_once() {
	local path="$1" bak
	bak="${path}.bak-install"
	if board "test -f '$bak'" >/dev/null 2>&1; then
		log "  백업 이미 있음, 덮어쓰지 않음: $bak"
		return 0
	fi
	if dry; then
		log "  [dry-run] 백업 생성 예정: $path -> $bak"
		return 0
	fi
	board "cp '$path' '$bak'" || die "백업 실패: $path"
	log "  백업 생성: $path -> $bak"
}

pull_file() { adb pull "$1" "$2" >/dev/null 2>&1; }

get_ntp_enable() {
	# $1 = 로컬로 pull한 ini 파일. [network.ntp] 섹션의 enable 값만 출력.
	awk '
		/^\[network\.ntp\]/ { insec=1; next }
		insec==1 && /^\[/ { exit }
		insec==1 && /^enable[ \t]*=/ {
			v=$0; gsub(/[ \t]/,"",v); sub(/^enable=/,"",v); print v; exit
		}
	' "$1"
}

set_ntp_enable_zero() {
	# $1 = 입력 ini, $2 = 출력 ini. [network.ntp] 섹션의 enable 줄 끝 "1"만 "0"으로.
	# 이미 0이면 패턴이 안 맞아 그대로 유지된다(멱등).
	awk '
		/^\[network\.ntp\]/ { insec=1; print; next }
		insec==1 && /^enable[ \t]*=/ { sub(/1[ \t]*$/, "0"); print; insec=0; next }
		{ print }
	' "$1" >"$2"
}

###############################################################################
# 3. 시계 수정 (rkipc NTP +8시간 하드코딩 오프셋)
###############################################################################
log "=== 3. 시계 수정 (factory ini + /userdata/rkipc.ini) ==="

pull_file "$FACTORY_INI" "$WORKDIR/factory.ini" || die "factory ini pull 실패: $FACTORY_INI"
FACTORY_BEFORE="$(get_ntp_enable "$WORKDIR/factory.ini")"
log "  $FACTORY_INI [network.ntp] enable = ${FACTORY_BEFORE:-<없음>}"

pull_file /userdata/rkipc.ini "$WORKDIR/userdata.ini" || die "/userdata/rkipc.ini pull 실패"
USERDATA_BEFORE="$(get_ntp_enable "$WORKDIR/userdata.ini")"
log "  /userdata/rkipc.ini [network.ntp] enable = ${USERDATA_BEFORE:-<없음>}"

if [ "$FACTORY_BEFORE" = "0" ] && [ "$USERDATA_BEFORE" = "0" ]; then
	log "  이미 적용됨 - 건너뜀 (rkipc 안 건드림)"
else
	if dry; then
		log "  [dry-run] rkipc 정지 후 두 파일의 enable을 0으로 바꿀 예정"
		CHANGED=1
	else
		RKIPC_PID="$(board pidof rkipc 2>/dev/null | tr -d '\r\n')"
		if [ -n "$RKIPC_PID" ]; then
			log "  rkipc 정지: SIGTERM -> pid $RKIPC_PID"
			board "kill -TERM $RKIPC_PID" >/dev/null 2>&1
			sleep 5
			if board "pidof rkipc" >/dev/null 2>&1; then
				warn "  rkipc가 5초 뒤에도 살아있음 - SIGKILL"
				board 'kill -KILL $(pidof rkipc)' >/dev/null 2>&1
				sleep 1
			fi
			if board "pidof rkipc" >/dev/null 2>&1; then
				die "rkipc를 정지하지 못함 - 중단"
			fi
			log "  rkipc 정지 확인됨"
		else
			log "  rkipc 이미 정지 상태"
		fi

		backup_once "$FACTORY_INI"
		backup_once /userdata/rkipc.ini

		set_ntp_enable_zero "$WORKDIR/factory.ini" "$WORKDIR/factory.ini.new"
		if ! diff -q "$WORKDIR/factory.ini" "$WORKDIR/factory.ini.new" >/dev/null; then
			adb push "$WORKDIR/factory.ini.new" "$FACTORY_INI" >/dev/null
			CHANGED=1
		fi

		set_ntp_enable_zero "$WORKDIR/userdata.ini" "$WORKDIR/userdata.ini.new"
		if ! diff -q "$WORKDIR/userdata.ini" "$WORKDIR/userdata.ini.new" >/dev/null; then
			adb push "$WORKDIR/userdata.ini.new" /userdata/rkipc.ini >/dev/null
			CHANGED=1
		fi

		FACTORY_AFTER="$(get_ntp_enable "$WORKDIR/factory.ini.new")"
		USERDATA_AFTER="$(get_ntp_enable "$WORKDIR/userdata.ini.new")"
		log "  $FACTORY_INI enable: $FACTORY_BEFORE -> $FACTORY_AFTER"
		log "  /userdata/rkipc.ini enable: $USERDATA_BEFORE -> $USERDATA_AFTER"
	fi
fi

###############################################################################
# 4. dhcpcd 설정 (wg* 제외)
###############################################################################
log "=== 4. dhcpcd 설정 ==="

pull_file /etc/dhcpcd.conf "$WORKDIR/dhcpcd.conf" || die "/etc/dhcpcd.conf pull 실패"

if grep -qxF 'denyinterfaces wg*' "$WORKDIR/dhcpcd.conf"; then
	log "  denyinterfaces wg* 이미 있음 - 건너뜀"
else
	if dry; then
		log "  [dry-run] /etc/dhcpcd.conf 끝에 'denyinterfaces wg*' 추가 예정"
		CHANGED=1
	else
		backup_once /etc/dhcpcd.conf
		cp "$WORKDIR/dhcpcd.conf" "$WORKDIR/dhcpcd.conf.new"
		printf 'denyinterfaces wg*\n' >>"$WORKDIR/dhcpcd.conf.new"
		adb push "$WORKDIR/dhcpcd.conf.new" /etc/dhcpcd.conf >/dev/null
		log "  denyinterfaces wg* 추가 완료"
		CHANGED=1
	fi
fi

###############################################################################
# 5. heartbeat 설치
###############################################################################
log "=== 5. heartbeat 설치 ==="

install_file_if_changed() {
	local src="$1" dst="$2" local_md5 remote_md5
	local_md5="$(md5sum "$src" | awk '{print $1}')"
	remote_md5="$(board "md5sum '$dst' 2>/dev/null" 2>/dev/null | awk '{print $1}')"

	if [ -n "$remote_md5" ] && [ "$local_md5" = "$remote_md5" ]; then
		log "  $dst: 이미 최신(md5 동일) - 건너뜀"
		return 0
	fi

	if dry; then
		log "  [dry-run] $dst 설치/교체 예정 (현재 md5=${remote_md5:-<없음>} -> $local_md5)"
		CHANGED=1
		return 0
	fi

	if [ -n "$remote_md5" ]; then
		backup_once "$dst"
	fi
	adb push "$src" "$dst" >/dev/null
	board "chmod 755 '$dst'"
	log "  $dst 설치 완료 (md5 ${remote_md5:-<없음>} -> $local_md5)"
	CHANGED=1
}

install_file_if_changed "$LOCAL_HEARTBEAT_BIN" /usr/bin/heartbeat
install_file_if_changed "$LOCAL_S60_SCRIPT" /etc/init.d/S60heartbeat

if ! dry; then
	board "chmod 755 /usr/bin/heartbeat /etc/init.d/S60heartbeat"
fi

###############################################################################
# 6. 마무리
###############################################################################
log "=== 6. 마무리 ==="

if dry; then
	log "[dry-run] sync 생략"
else
	board sync >/dev/null 2>&1
	log "sync 완료"
fi

if [ "$CHANGED" = "1" ]; then
	if dry; then
		log "REBOOT_REQUIRED: 예 (dry-run 기준 예상 - 실제 적용 시 재부팅 필요) - 자동 재부팅 안 함"
	else
		log "REBOOT_REQUIRED: 예 - 자동 재부팅 안 함, 직접 'adb reboot' 실행할 것"
	fi
else
	log "REBOOT_REQUIRED: 아니오 (바뀐 것 없음)"
fi

log "완료"

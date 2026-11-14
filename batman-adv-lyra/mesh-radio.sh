#!/bin/sh
# The mesh radio: Wi-Fi channel and TX power cap, changed at runtime and kept in
# /etc/default/mesh (MESH_FREQ, MESH_TXPOWER) for the next boot (mesh-up).
#   mesh-radio get                  JSON: channel, frequency, TX power cap, channels allowed here
#   mesh-radio check <channel>      is the channel usable here? (prints why not)
#   mesh-radio channel <channel>    join the mesh cell on that channel (back on the old one if it fails)
#   mesh-radio txpower <dBm|off>    cap the TX power (rtw88 honours only its SAR limit, not iw txpower)
# Every node of the mesh must be on the same channel: the web UI switches them together
# (cgi-bin/radio, scope=all).
# MESH_COUNTRY picks the regulatory rules: PA (Panama) has no radar-detection (DFS) or
# no-initiate (no IR) limits anywhere, so 2.4 GHz 1-13 and 5 GHz 36-64, 100-140, 149-165 can all
# carry the ad-hoc mesh; the world rules (00) would allow only 1-11.
CONF=/etc/default/mesh
[ -r "$CONF" ] && . "$CONF"
MESH_SSID=${MESH_SSID:-LyraMesh}
MESH_BSSID=${MESH_BSSID:-02:ca:fe:ca:ca:40}
MESH_COUNTRY=${MESH_COUNTRY:-PA}
TXPOWER_MAX=20

IF=$(batctl if 2>/dev/null | head -n1 | cut -d: -f1)
[ -n "$IF" ] || IF=$(ls /sys/class/net | grep '^wlx' | head -n1)
PHY=$(cat "/sys/class/net/$IF/phy80211/name" 2>/dev/null)

# The kernel applies a country asynchronously: wait until it is in effect (up to 5 s)
country() {
	iw reg get 2>/dev/null | awk '/^country/ { sub(":", "", $2); print $2; exit }'
}
ensure_country() {
	[ "$(country)" = "$MESH_COUNTRY" ] && return 0
	iw reg set "$MESH_COUNTRY" 2>/dev/null
	for _ in 1 2 3 4 5 6 7 8 9 10; do
		[ "$(country)" = "$MESH_COUNTRY" ] && return 0
		sleep 0.5
	done
	return 1
}

# "channel frequency" of every channel the regulatory rules let this node start an IBSS on
# (not disabled, no "no IR", no radar detection)
channels() {
	iw phy "$PHY" info 2>/dev/null | awk '/ MHz \[[0-9]+\]/ && !/disabled|no IR|radar/ {
		ch = $4; gsub(/[][]/, "", ch); print ch, $2 }'
}

freq_of() {
	channels | awk -v c="$1" '$1 == c { print $2; exit }'
}

current_freq() {
	iw dev "$IF" info 2>/dev/null | awk '/channel/ { gsub(/\(/, "", $3); print $3; exit }'
}

set_conf() {
	[ -e "$CONF" ] || touch "$CONF"
	if grep -q "^$1=" "$CONF"; then
		sed -i "s|^$1=.*|$1=$2|" "$CONF"
	else
		echo "$1=$2" >> "$CONF"
	fi
	sync
}

check() {
	echo "$1" | grep -Eqx '[0-9]{1,3}' || { echo "the channel must be a number, e.g. 6"; return 1; }
	[ -n "$IF" ] && [ -n "$PHY" ] || { echo "no mesh radio"; return 1; }
	[ -n "$(freq_of "$1")" ] || {
		echo "channel $1 is not allowed here (regulatory rules: $(channels | awk '{printf "%s%s", s, $1; s=", "}'))"
		return 1
	}
}

join() {
	iw dev "$IF" ibss leave 2>/dev/null
	iw dev "$IF" ibss join "$MESH_SSID" "$1" HT20 fixed-freq "$MESH_BSSID" 2>/dev/null || return 1
	# the join completes asynchronously
	for _ in 1 2 3 4 5; do
		[ "$(current_freq)" = "$1" ] && return 0
		sleep 1
	done
	return 1
}

ensure_country

case "$1" in
get)
	freq=$(current_freq)
	ch=$(channels | awk -v f="$freq" '$2 == f { print $1; exit }')
	list=$(channels | awk '{ printf "%s%s", s, $1; s = "," }')
	cap=${MESH_TXPOWER:-null}
	printf '{"ok":true,"interface":"%s","channel":%s,"freq":%s,"country":"%s","txpower_cap_dbm":%s,"txpower_max_dbm":%s,"channels":[%s]}\n' \
		"$IF" "${ch:-null}" "${freq:-null}" "$(country)" "$cap" "$TXPOWER_MAX" "$list"
	;;
check)
	check "$2"
	;;
channel)
	why=$(check "$2") || { echo "$why" >&2; exit 1; }
	new=$(freq_of "$2")
	old=$(current_freq)
	[ "$new" = "$old" ] && { set_conf MESH_FREQ "$new"; exit 0; }
	if join "$new"; then
		set_conf MESH_FREQ "$new"
		logger -t mesh-radio "channel ${old:-?} MHz -> $new MHz (channel $2)"
	else
		logger -t mesh-radio "channel $2 ($new MHz) failed, back to ${old:-2437} MHz"
		join "${old:-2437}"
		echo "could not join the mesh on channel $2" >&2
		exit 1
	fi
	;;
txpower)
	case "$2" in
	off | "") dbm=$TXPOWER_MAX; keep="" ;;
	*)
		echo "$2" | grep -Eqx '[0-9]{1,2}' && [ "$2" -ge 1 ] && [ "$2" -le $TXPOWER_MAX ] ||
			{ echo "TX power must be 1..$TXPOWER_MAX dBm or off" >&2; exit 1; }
		dbm=$2; keep=$2
		;;
	esac
	q=$((dbm * 4))      # SAR limits are in 1/4 dBm, bands 0-3
	iw phy "$PHY" set sar_specs 0 0:$q 1:$q 2:$q 3:$q || { echo "the driver refused the TX power limit" >&2; exit 1; }
	set_conf MESH_TXPOWER "$keep"
	logger -t mesh-radio "TX power cap ${keep:-off} (${dbm} dBm)"
	;;
*)
	echo "usage: $0 get | check <channel> | channel <channel> | txpower <dBm|off>" >&2
	exit 2
	;;
esac

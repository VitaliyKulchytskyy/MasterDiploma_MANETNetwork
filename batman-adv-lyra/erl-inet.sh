#!/bin/sh
# Internet status of this node for the web UI (erl-inet.service), in /run/erl/internet.json:
# role (MESH_GW), own uplink (phone USB tethering, Wi-Fi, ...), default route, mesh gateway in
# use, DNS, and a real reachability check. On a gateway (MESH_GW=server) it also keeps the
# batman-adv announcement in step with the uplink (mesh-gw announce).
STATE_DIR=/run/erl
STATE=$STATE_DIR/internet.json
INTERVAL=5          # route/uplink/announcement
CHECK_EVERY=6       # reachability check every INTERVAL * CHECK_EVERY seconds
PING_HOST=1.1.1.1
DNS_NAME=example.com

NTP_NUDGE_EVERY=120  # at most one timesyncd restart per this many seconds

mkdir -p "$STATE_DIR"

uptime_s() {
	cut -d. -f1 /proc/uptime
}

# The board has no RTC: after boot the clock is whatever was saved at the last shutdown, until
# systemd-timesyncd reaches an NTP server. It retries on its own timer and does not notice an
# uplink appearing (phone tethering, a mesh gateway): once the internet is reachable and the
# clock is still not synchronized, restart it so it syncs within seconds.
clock_sync() {
	[ "$(timedatectl show -p NTPSynchronized --value 2>/dev/null)" = yes ] && return 0
	systemctl is-enabled -q systemd-timesyncd 2>/dev/null || return 0
	[ $(($(uptime_s) - last_nudge)) -ge $NTP_NUDGE_EVERY ] || return 0
	last_nudge=$(uptime_s)
	systemctl restart systemd-timesyncd
}

# {"synced":true,"server":"ntp.ubuntu.com"} for the UI
clock_json() {
	synced=false
	[ "$(timedatectl show -p NTPSynchronized --value 2>/dev/null)" = yes ] && synced=true
	server=$(timedatectl show-timesync -p ServerName --value 2>/dev/null)
	printf '{"synced":%s,"server":%s}' "$synced" "$(json_str "$server")"
}

# /etc/default/mesh, or for now $STATE_DIR/gw-role (pc-link.sh --share makes a node the gateway
# while it shares a PC's internet)
role() {
	[ -r "$STATE_DIR/gw-role" ] && { cat "$STATE_DIR/gw-role"; return; }
	MESH_GW=off
	[ -r /etc/default/mesh ] && . /etc/default/mesh
	echo "${MESH_GW:-off}"
}

# what kind of link an interface is, for a short label in the UI
link_kind() {
	# set by whoever made this link the uplink (pc-link.sh --share on a PC: "pc")
	[ -r "$STATE_DIR/uplink-kind-$1" ] && { cat "$STATE_DIR/uplink-kind-$1"; return; }
	# the phone link itself (usb-ncm gadget): the phone's Ethernet tethering (usb-uplink)
	NCM_IF=
	[ -r /run/usb-ncm.env ] && . /run/usb-ncm.env
	[ "$1" = "$NCM_IF" ] && { echo phone-eth; return; }
	driver=$(basename "$(readlink "/sys/class/net/$1/device/driver" 2>/dev/null)" 2>/dev/null)
	case "$driver" in
	rndis_host|cdc_ncm|cdc_ether) echo phone ;;
	*) [ -d "/sys/class/net/$1/wireless" ] && echo wifi || echo "$1" ;;
	esac
}

json_str() {
	[ -n "$1" ] && printf '"%s"' "$1" || printf null
}

online=null
latency=null
dns_ok=null
checked=null
clock=null
last_nudge=-$NTP_NUDGE_EVERY
n=0

while :; do
	ROLE=$(role)
	# the preferred (lowest metric) default route
	route=$(ip -4 route show default | head -n1)
	via=$(echo "$route" | awk '{for (i = 1; i < NF; i++) if ($i == "via") {print $(i + 1); exit}}')
	dev=$(echo "$route" | awk '{for (i = 1; i < NF; i++) if ($i == "dev") {print $(i + 1); exit}}')

	gateway=""
	uplink=""
	if [ "$dev" = bat0 ]; then
		gateway=$via                                    # internet through a mesh gateway
	elif [ -n "$dev" ]; then
		uplink=$dev                                     # own uplink
	fi

	uplink_json=null
	if [ -n "$uplink" ]; then
		# the route's src: on the phone link that is the phone-given address, not 192.168.7.1
		addr=$(echo "$route" | awk '{for (i = 1; i < NF; i++) if ($i == "src") {print $(i + 1); exit}}')
		[ -n "$addr" ] || addr=$(ip -4 -o addr show "$uplink" | awk '{print $4; exit}')
		uplink_json=$(printf '{"dev":"%s","kind":"%s","address":%s,"via":%s}' \
			"$uplink" "$(link_kind "$uplink")" "$(json_str "$addr")" "$(json_str "$via")")
	fi

	announced=false
	if [ "$ROLE" = server ] && [ -x /usr/local/sbin/mesh-gw ] && [ -d /sys/class/net/bat0 ]; then
		mode=$(/usr/local/sbin/mesh-gw announce 2>/dev/null)
		[ "$mode" = server ] && announced=true
		# no uplink of its own: a client of the mesh's other gateways for now
		[ "$mode" = client ] && ! systemctl is-active --quiet mesh-gwclient && systemctl start mesh-gwclient
	fi

	# real reachability: only when there is a route at all
	if [ $((n % CHECK_EVERY)) -eq 0 ]; then
		checked=$(date +%s)
		if [ -n "$dev" ]; then
			rtt=$(ping -n -c 1 -W 3 $PING_HOST 2>/dev/null | sed -n 's/.*time=\([0-9.]*\) ms.*/\1/p')
			if [ -n "$rtt" ]; then
				online=true
				latency=$(printf '%.0f' "$rtt")
			else
				online=false
				latency=null
			fi
			timeout 5 getent hosts $DNS_NAME >/dev/null 2>&1 && dns_ok=true || dns_ok=false
			[ "$online" = true ] && [ "$dns_ok" = true ] && clock_sync
		else
			online=false
			latency=null
			dns_ok=false
		fi
		clock=$(clock_json)
	fi
	n=$((n + 1))

	nameservers=$(awk '/^nameserver/ {printf "%s\"%s\"", sep, $2; sep = ","}' /run/systemd/resolve/resolv.conf 2>/dev/null)

	printf '{"role":"%s","online":%s,"latency_ms":%s,"dns":%s,"checked":%s,"uplink":%s,"gateway":%s,"announced":%s,"nameservers":[%s],"clock":%s,"updated":%s}\n' \
		"$ROLE" "$online" "$latency" "$dns_ok" "$checked" "$uplink_json" "$(json_str "$gateway")" "$announced" \
		"$nameservers" "$clock" "$(date +%s)" > "$STATE.tmp" && mv "$STATE.tmp" "$STATE"

	sleep $INTERVAL
done

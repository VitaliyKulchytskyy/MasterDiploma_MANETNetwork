#!/usr/bin/env bash
# pc-link.sh: USB Ethernet between this PC and an ERL node (Luckfox Lyra) plugged into it.
#
# Plugged into a PC, the node's USB gadget shows two network adapters here: RNDIS (the
# board's usb0, 192.168.123.x, Luckfox's own) and the phone link (CDC-ECM/NCM, the board's
# usb1, 192.168.7.1), where the node's dnsmasq hands out 192.168.7.10-50 without a router, so
# this PC keeps its own internet. This script uses the phone link.
#
#   pc-link.sh up [--share] [-s SERIAL]   address on the link, check the node answers;
#                                         --share: also give the node this PC's internet
#   pc-link.sh down [-s SERIAL]           stop sharing (the link itself stays up)
#   pc-link.sh status [-s SERIAL]
#
# --share NATs the node's link out of this PC's default route and, over adb, points the
# node's default route at this PC. The node also becomes the mesh's internet gateway while it
# shares (whatever its MESH_GW: needs the erl kernel), like with the phone's Ethernet
# tethering; `down` puts its own role back. Needs sudo (iptables, sysctl).
# -s picks the node by its adb serial when several are plugged in (the USB serial is the same).
set -euo pipefail

NODE_IP=192.168.7.1
SUBNET=192.168.7.0/24
STATIC_IP=192.168.7.2/24        # fallback when no DHCP client answers; outside the node's pool
TAG=erl-pc-share                # iptables comment of the rules this script adds

die() { echo "pc-link: $*" >&2; exit 1; }
say() { echo "pc-link: $*"; }

# "<ifname> <usb serial>" for every phone-link adapter of a Rockchip gadget (vendor 2207)
node_links() {
	local n dev usb drv
	for n in /sys/class/net/*; do
		dev=$(readlink -f "$n/device" 2>/dev/null) || continue
		usb=$(dirname "$dev")
		[ "$(cat "$usb/idVendor" 2>/dev/null)" = 2207 ] || continue
		drv=$(basename "$(readlink "$n/device/driver" 2>/dev/null)")
		case "$drv" in cdc_ether | cdc_ncm) echo "$(basename "$n") $(cat "$usb/serial" 2>/dev/null)" ;; esac
	done
}

pick_link() {
	local links
	links=$(node_links | { [ -n "$SERIAL" ] && awk -v s="$SERIAL" '$2 == s' || cat; })
	case $(echo "$links" | grep -c .) in
	0) die "no ERL node's USB Ethernet here${SERIAL:+ with serial $SERIAL} (plugged in? usb-ncm running on it?)" ;;
	1) read -r IF SERIAL <<<"$links" ;;
	*) die "several nodes plugged in, pick one with -s:$(echo; echo "$links" | sed 's/^/  /')" ;;
	esac
}

link_ip() {
	ip -4 -o addr show dev "$IF" | awk '{print $4}' | grep '^192\.168\.7\.' | head -n1 | cut -d/ -f1
}

# The node's dnsmasq answers DHCP on the link; NetworkManager usually does this by itself
get_address() {
	[ -n "$(link_ip)" ] && return 0
	ip link show "$IF" | grep -q "state UP" || sudo ip link set "$IF" up
	if command -v nmcli >/dev/null && [ "$(nmcli -g GENERAL.STATE dev show "$IF" 2>/dev/null)" != "" ]; then
		nmcli dev set "$IF" managed yes 2>/dev/null || true
		nmcli dev connect "$IF" >/dev/null 2>&1 || true
	elif command -v dhclient >/dev/null; then
		sudo timeout 15 dhclient -1 "$IF" 2>/dev/null || true
	elif command -v udhcpc >/dev/null; then
		sudo udhcpc -i "$IF" -n -q -t 5 2>/dev/null || true
	fi
	for _ in $(seq 15); do
		[ -n "$(link_ip)" ] && return 0
		sleep 1
	done
	say "no DHCP answer, using $STATIC_IP"
	sudo ip addr add "$STATIC_IP" dev "$IF"
}

# This PC's own internet: the default route not over the node's link
wan_dev() {
	ip -4 route show default | awk -v l="$IF" '{for (i = 1; i < NF; i++) if ($i == "dev" && $(i + 1) != l) {print $(i + 1); exit}}'
}

rule() {
	local table=$1 chain=$2; shift 2
	sudo iptables -t "$table" -C "$chain" "$@" -m comment --comment $TAG 2>/dev/null ||
		sudo iptables -t "$table" -I "$chain" "$@" -m comment --comment $TAG
}

share_up() {
	local wan pc_ip
	wan=$(wan_dev)
	[ -n "$wan" ] || die "this PC has no default route to share"
	pc_ip=$(link_ip)
	command -v adb >/dev/null && adb -s "$SERIAL" get-state >/dev/null 2>&1 ||
		die "adb cannot reach the node $SERIAL: needed to point its default route at this PC"

	sudo sysctl -q -w net.ipv4.ip_forward=1
	rule nat POSTROUTING -s $SUBNET -o "$wan" -j MASQUERADE
	# Docker & co. set the FORWARD policy to DROP: let the node's traffic through explicitly
	rule filter FORWARD -i "$IF" -o "$wan" -j ACCEPT
	rule filter FORWARD -i "$wan" -o "$IF" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT

	# on the node: default route via this PC (metric 50: before phone tethering 100 and a mesh
	# gateway 600), public DNS, and tell erl-inet the uplink is a PC (web UI label)
	adb -s "$SERIAL" shell ". /run/usb-ncm.env && ip route replace default via $pc_ip dev \$NCM_IF metric 50 &&
		resolvectl dns \$NCM_IF 1.1.1.1 8.8.8.8 && resolvectl domain \$NCM_IF '~.' &&
		mkdir -p /run/erl && echo pc > /run/erl/uplink-kind-\$NCM_IF"
	say "sharing $wan with the node (NAT on this PC, node's default route via $pc_ip)"

	# the node shares it with the mesh: gateway for now (a client node would only use it itself)
	adb -s "$SERIAL" shell '. /etc/default/mesh 2>/dev/null
		if [ "$MESH_GW" != server ]; then
			echo server > /run/erl/gw-role
			systemctl stop mesh-gwclient 2>/dev/null
			# the default route and DNS a mesh gateway gave this node as a client
			ip route del default dev bat0 2>/dev/null; resolvectl revert bat0 2>/dev/null
			systemctl start mesh-gw mesh-gw-dns
		fi
		/usr/local/sbin/mesh-gw announce' | tr -d '\r' | grep -qx server &&
		say "the node is the mesh's internet gateway now" ||
		say "the node could not become the mesh gateway (erl kernel? mesh up?)"
}

share_down() {
	local spec
	for table in nat filter; do
		sudo iptables -t "$table" -S 2>/dev/null | grep -- "--comment $TAG" | sed 's/^-[AI] /-D /' |
			while read -r spec; do eval sudo iptables -t "$table" "$spec"; done
	done
	if command -v adb >/dev/null && adb -s "$SERIAL" get-state >/dev/null 2>&1; then
		adb -s "$SERIAL" shell '. /run/usb-ncm.env; f=/run/erl/uplink-kind-$NCM_IF
			if [ "$(cat $f 2>/dev/null)" = pc ]; then
				ip route del default dev $NCM_IF metric 50 2>/dev/null; resolvectl revert $NCM_IF; rm -f $f
			fi
			# the gateway role was only for sharing: back to its own (MESH_GW)
			if [ -e /run/erl/gw-role ]; then
				rm -f /run/erl/gw-role
				systemctl stop mesh-gw-dns mesh-gw
				. /etc/default/mesh 2>/dev/null
				[ "$MESH_GW" = client ] && batctl gw_mode client && systemctl start mesh-gwclient
			fi'
	else
		say "adb cannot reach the node: its default route via this PC stays until it reboots"
	fi
	say "not sharing any more"
}

status() {
	local pc_ip
	pc_ip=$(link_ip)
	echo "node link:  $IF (node $SERIAL)"
	echo "this PC:    ${pc_ip:-no address}"
	if ping -c1 -W2 "$NODE_IP" >/dev/null 2>&1; then
		echo "node:       $NODE_IP answers, web UI http://$NODE_IP/"
	else
		echo "node:       $NODE_IP does not answer"
	fi
	if sudo -n iptables -t nat -S 2>/dev/null | grep -q -- "--comment $TAG"; then
		echo "sharing:    on (via $(wan_dev))"
	else
		echo "sharing:    off (or run with sudo rights to check)"
	fi
	if command -v adb >/dev/null && adb -s "$SERIAL" get-state >/dev/null 2>&1; then
		echo "node route: $(adb -s "$SERIAL" shell 'ip -4 route show default | head -n1' | tr -d '\r')"
		echo "node inet:  $(adb -s "$SERIAL" shell 'cat /run/erl/internet.json 2>/dev/null' | tr -d '\r' |
			sed -n 's/.*"online":\([a-z]*\).*/online=\1/p')"
	fi
}

cmd=${1:-}; shift || true
SHARE=0
SERIAL=""
while [ $# -gt 0 ]; do
	case "$1" in
	--share) SHARE=1 ;;
	-s) SERIAL=${2:?-s needs a serial}; shift ;;
	*) die "unknown option $1" ;;
	esac
	shift
done

case "$cmd" in
up)
	pick_link
	get_address
	for _ in $(seq 10); do ping -c1 -W1 "$NODE_IP" >/dev/null 2>&1 && break; sleep 1; done
	ping -c1 -W2 "$NODE_IP" >/dev/null 2>&1 || die "$IF is up ($(link_ip)) but $NODE_IP does not answer"
	say "$IF $(link_ip) -> node $NODE_IP, web UI http://$NODE_IP/"
	if [ $SHARE = 1 ]; then share_up; fi
	;;
down)
	pick_link
	share_down
	;;
status)
	pick_link
	status
	;;
*)
	sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
	;;
esac

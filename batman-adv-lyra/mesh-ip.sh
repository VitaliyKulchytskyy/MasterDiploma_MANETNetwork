#!/bin/sh
# This node's mesh IP (bat0), changed at runtime and kept in /etc/default/mesh (MESH_IP).
# Used by the web UI (cgi-bin/mesh-ip), also by hand:
#   mesh-ip check <a.b.c.d/nn>   is it a valid address for this node? (prints why not)
#   mesh-ip set <a.b.c.d/nn>     check, save, and move bat0 to it (dependent services restarted)
CONF=/etc/default/mesh

current() {
	ip -4 -o addr show bat0 2>/dev/null | awk '{print $4; exit}'
}

# a.b.c.d/nn: octets 0-255, prefix 8-30, not the subnet's own or broadcast address
check() {
	echo "$1" | grep -Eqx '([0-9]{1,3}\.){3}[0-9]{1,3}/[0-9]{1,2}' ||
		{ echo "use the form a.b.c.d/nn, e.g. 10.10.0.3/24"; return 1; }
	addr=${1%/*}
	prefix=${1#*/}
	[ "$prefix" -ge 8 ] && [ "$prefix" -le 30 ] || { echo "the prefix must be 8..30"; return 1; }
	value=0
	for o in $(echo "$addr" | tr . ' '); do
		# no leading zeros: "010" would be octal to the shell and ambiguous to people
		case "$o" in 0?*) echo "no leading zeros in the address"; return 1 ;; esac
		[ "$o" -le 255 ] || { echo "each part of the address must be 0..255"; return 1; }
		value=$((value * 256 + o))
	done
	host_mask=$(((1 << (32 - prefix)) - 1))
	host=$((value & host_mask))
	[ "$host" -ne 0 ] || { echo "that is the subnet's own address, pick another host"; return 1; }
	[ "$host" -ne "$host_mask" ] || { echo "that is the subnet's broadcast address, pick another host"; return 1; }
	# another node already on it (only a new address: our own answers too)
	if [ "${addr}" != "$(current | cut -d/ -f1)" ] && ping -c 1 -W 1 -I bat0 "$addr" >/dev/null 2>&1; then
		echo "$addr is already used by another node in the mesh"
		return 1
	fi
	return 0
}

set_ip() {
	new=$1
	old=$(current)
	[ -e "$CONF" ] || touch "$CONF"
	if grep -q '^MESH_IP=' "$CONF"; then
		sed -i "s|^MESH_IP=.*|MESH_IP=$new|" "$CONF"
	else
		echo "MESH_IP=$new" >> "$CONF"
	fi
	sync
	[ "$new" = "$old" ] && return 0
	ip addr add "$new" dev bat0 2>/dev/null || ip addr replace "$new" dev bat0
	[ -n "$old" ] && ip addr del "$old" dev bat0 2>/dev/null
	# tell the other nodes at once (gratuitous ARP): their caches then have the new address as
	# the current one, and the web UIs show it instead of the old
	busybox arping -q -U -c 2 -I bat0 "${new%/*}" 2>/dev/null || true
	# NAT and the mesh DHCP range follow the subnet / address
	for s in mesh-gw mesh-gw-dns; do
		systemctl -q is-active "$s" && systemctl restart "$s"
	done
	systemctl -q is-active erl-inet && systemctl restart erl-inet
	logger -t mesh-ip "bat0 ${old:-none} -> $new"
}

case "$1" in
check)
	check "$2"
	;;
set)
	why=$(check "$2") || { echo "$why" >&2; exit 1; }
	set_ip "$2"
	;;
*)
	echo "usage: $0 check|set <a.b.c.d/nn>" >&2
	exit 2
	;;
esac

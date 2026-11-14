#!/bin/sh
# Internet gateway for the mesh (MESH_GW=server): NAT the mesh subnet out of whatever
# uplink currently has the default route (phone USB tethering, ...), and announce the
# gateway to batman-adv. Needs the -erl kernel (netfilter/NAT) and iptables.
# Usage: mesh-gw up | down | dns | announce
set -e

COMMENT=mesh-gw

# Default route that does not go through the mesh: this node's own uplink (phone, ...)
uplink_dev() {
	ip -4 route show default | awk '$0 !~ / dev bat0( |$)/ { for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }'
}

# Announce the gateway in the mesh only while there is an uplink, so nodes do not
# pick a gateway without internet (called by erl-inet on every check). Without one, use
# another gateway's instead (client: mesh-gwclient takes its route, metric 600, so an uplink
# that comes back still wins, and uplink_dev() above leaves bat0 routes out).
announce() {
	want=client
	[ -n "$(uplink_dev)" ] && want=server
	have=$(batctl gw_mode 2>/dev/null | awk '{print $1}')
	[ "$have" = "$want" ] || batctl gw_mode $want
	echo $want
}

# bat0's connected route, e.g. 10.10.0.0/24
mesh_subnet() {
	ip -4 route show dev bat0 proto kernel scope link | awk '{print $1; exit}'
}

rule() {
	# idempotent: add a rule only if it is not there yet
	table=$1; chain=$2; shift 2
	iptables -t "$table" -C "$chain" "$@" -m comment --comment $COMMENT 2>/dev/null ||
		iptables -t "$table" -A "$chain" "$@" -m comment --comment $COMMENT
}

flush_rules() {
	for table in nat mangle; do
		iptables -t "$table" -S 2>/dev/null | grep -- "--comment $COMMENT" | sed 's/^-A /-D /' |
			while read -r spec; do eval iptables -t "$table" "$spec"; done
	done
}

case "$1" in
up)
	SUBNET=$(mesh_subnet)
	UPLINK=$(uplink_dev)
	[ -n "$SUBNET" ] || { echo "bat0 has no address, is the mesh up?" >&2; exit 1; }
	sysctl -q -w net.ipv4.ip_forward=1
	# mesh -> anywhere outside the mesh leaves with the uplink's address
	rule nat POSTROUTING -s "$SUBNET" ! -d "$SUBNET" -j MASQUERADE
	# the mesh (batman-adv), the phone link and the uplink have different MTUs
	rule mangle FORWARD -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu
	echo "mesh-gw up: NAT $SUBNET, uplink: ${UPLINK:-none yet}, announced: $(announce)"
	;;
down)
	flush_rules
	# ip_forward stays on: the phone link (usb-ncm) may still share this node's route
	batctl gw_mode off 2>/dev/null || true
	echo "mesh-gw down"
	;;
announce)
	announce
	;;
dns)
	# DHCP + DNS on bat0 for the other nodes (mesh-gw-dns.service). They keep their static
	# mesh IPs and only take the router (this node) and DNS options from the lease; the
	# pool .200-.250 of the mesh /24 is only used for that exchange.
	ADDR=$(ip -4 -o addr show bat0 | awk '{print $4; exit}')
	IP=${ADDR%/*}
	[ -n "$IP" ] || { echo "bat0 has no address, is the mesh up?" >&2; exit 1; }
	NET=${IP%.*}
	exec dnsmasq -k --interface=bat0 --bind-interfaces --except-interface=lo --no-hosts \
		--resolv-file=/run/systemd/resolve/resolv.conf --dhcp-leasefile=/run/mesh-gw.leases \
		--dhcp-range="$NET.200,$NET.250,255.255.255.0,10m" \
		--dhcp-option=3,"$IP" --dhcp-option=6,"$IP"
	;;
*)
	echo "usage: $0 up|down|dns|announce" >&2
	exit 1
	;;
esac

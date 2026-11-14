#!/bin/sh
# Add a USB Ethernet function for the phone to the existing Rockchip USB gadget
# (adb + rndis), so a phone plugged into the OTG port sees a USB Ethernet adapter.
# Usage: usb-ncm up | down
#
# PHONE_FN (from /etc/default/usb-phone) picks the USB Ethernet class:
#   ecm - CDC-ECM (default): older Android (cdc_ether), newer Android, Linux,
#         macOS, iOS; not Windows
#   ncm - CDC-NCM: newer Android, Linux, macOS, iOS, Windows 10+; many
#         Android 11-era phone kernels lack its driver
set -e

[ -r /etc/default/usb-phone ] && . /etc/default/usb-phone
PHONE_FN=${PHONE_FN:-ecm}
case "$PHONE_FN" in
ecm|ncm) ;;
*) echo "PHONE_FN must be ecm or ncm, got '$PHONE_FN'" >&2; exit 1 ;;
esac

G=/sys/kernel/config/usb_gadget/rockchip
CFG=$G/configs/b.1
FN=$G/functions/$PHONE_FN.phone
LINK=$CFG/f9
ENV=/run/usb-ncm.env
NCM_IP=${NCM_IP:-192.168.7.1/24}

# Unbind, run the change, rebind. adb rides on the same gadget, so if the new
# config does not bind (e.g. the controller runs out of endpoints) roll back
# instead of leaving the board without USB.
rebind() {
	change=$1
	undo=$2
	udc=$(cat "$G/UDC")
	[ -n "$udc" ] || udc=$(ls /sys/class/udc | head -n1)
	echo "" > "$G/UDC" 2>/dev/null || true
	if $change && echo "$udc" > "$G/UDC"; then
		return 0
	fi
	echo "usb-ncm: new gadget config failed to bind, rolling back" >&2
	echo "" > "$G/UDC" 2>/dev/null || true
	$undo || true
	echo "$udc" > "$G/UDC"
	return 1
}

# Share this board's internet (a mesh gateway or its own uplink) with the phone: NAT the
# phone link out of whatever the default route is. Android then validates the USB link and
# prefers it over mobile data, so the web UI stays reachable in the browser. Needs the -erl
# kernel (netfilter); on the stock kernel the phone link simply has no internet.
SHARE_COMMENT=phone-share

share_rule() {
	table=$1; chain=$2; shift 2
	iptables -t "$table" -C "$chain" "$@" -m comment --comment $SHARE_COMMENT 2>/dev/null ||
		iptables -t "$table" -A "$chain" "$@" -m comment --comment $SHARE_COMMENT
}

share_up() {
	command -v iptables >/dev/null && iptables -t nat -S >/dev/null 2>&1 || {
		echo "usb-ncm: no NAT support (stock kernel), the phone link has no internet"
		return 0
	}
	subnet=$(ip -4 route show dev "$1" proto kernel scope link | awk '{print $1; exit}')
	sysctl -q -w net.ipv4.ip_forward=1
	share_rule nat POSTROUTING -s "$subnet" ! -d "$subnet" -j MASQUERADE
	share_rule mangle FORWARD -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu
	echo "usb-ncm: sharing internet with $subnet"
}

share_down() {
	command -v iptables >/dev/null || return 0
	for table in nat mangle; do
		iptables -t "$table" -S 2>/dev/null | grep -- "--comment $SHARE_COMMENT" | sed 's/^-A /-D /' |
			while read -r spec; do eval iptables -t "$table" "$spec"; done
	done
}

link_fn() {
	ln -s "$FN" "$LINK"
}

unlink_fn() {
	OLD=$(readlink -f "$LINK")
	rm -f "$LINK"
}

relink_old() {
	ln -s "$OLD" "$LINK"
}

case "$1" in
up)
	[ -d "$G" ] || { echo "gadget $G not set up yet" >&2; exit 1; }
	modprobe "usb_f_$PHONE_FN"
	[ -d "$FN" ] || mkdir "$FN"
	# A link to the other class (PHONE_FN changed): drop it first
	if [ -L "$LINK" ] && [ "$(readlink -f "$LINK")" != "$(readlink -f "$FN")" ]; then
		rebind unlink_fn relink_old
	fi
	if [ ! -L "$LINK" ]; then
		# Stable MACs derived from the gadget serial. host_addr (the phone's side)
		# must NOT be locally administered: with that bit set Linux hosts name the
		# link usb0 instead of eth0, and Android only brings up eth* adapters.
		sum=$(md5sum "$G/strings/0x409/serialnumber" | cut -c1-10)
		mac=$(echo "$sum" | sed 's/\(..\)\(..\)\(..\)\(..\)\(..\)/\1:\2:\3:\4:\5/')
		echo "02:$mac" > "$FN/dev_addr"
		echo "00:$mac" > "$FN/host_addr"
		rebind link_fn unlink_fn
	fi

	# After a switch, the other class's interface still exists: free the address
	for other in ecm ncm; do
		[ "$other" != "$PHONE_FN" ] && [ -d "$G/functions/$other.phone" ] &&
			ip addr flush dev "$(cat "$G/functions/$other.phone/ifname")" 2>/dev/null || true
	done

	IF=$(cat "$FN/ifname")
	nmcli dev set "$IF" managed no 2>/dev/null || true
	ip addr replace "$NCM_IP" dev "$IF"
	ip link set "$IF" up
	echo "NCM_IF=$IF" > "$ENV"
	share_up "$IF"
	echo "usb-ncm up: $PHONE_FN $IF $NCM_IP"
	;;
down)
	share_down
	[ -L "$LINK" ] && rebind unlink_fn relink_old
	rm -f "$ENV"
	echo "usb-ncm down"
	;;
*)
	echo "usage: $0 up|down" >&2
	exit 1
	;;
esac

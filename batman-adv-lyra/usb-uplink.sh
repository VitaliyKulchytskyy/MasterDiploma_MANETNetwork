#!/bin/sh
# Internet from the user's phone over the USB link, through the phone's *Ethernet tethering*
# (usb-uplink.service). The phone link is a USB gadget (usb-ncm), so to the phone the board is
# a USB Ethernet adapter: with Ethernet tethering on, the phone routes its mobile data (or
# Wi-Fi) out through it and runs a DHCP server on the link. This service keeps a DHCP client
# on the link; when the phone answers, the board takes an address in the phone's subnet (next
# to its own 192.168.7.1) and a default route through the phone. That default route is the
# uplink erl-inet / mesh-gw see, so a gateway node then shares it with the mesh.
#
# Usage: usb-uplink run            main loop (the service)
#        usb-uplink <udhcpc event> udhcpc hook (bound, renew, deconfig, leasefail, nak)
METRIC=100          # below the mesh gateway route (mesh-gwclient, 600): own uplink wins
STATE=/run/usb-uplink.state
CHECK=10            # seconds between checks that the phone still answers
MISSES=3            # unanswered checks before the uplink is dropped (tethering turned off)
SELF=/usr/local/sbin/usb-uplink

log() { logger -t usb-uplink "$*"; echo "usb-uplink: $*"; }

# Remove what the last lease configured; the link's own 192.168.7.1 is never touched
drop() {
	[ -r "$STATE" ] || return 0
	. "$STATE"
	ip route del default via "$ROUTER" dev "$IF" 2>/dev/null
	ip addr del "$ADDR" dev "$IF" 2>/dev/null
	resolvectl revert "$IF" 2>/dev/null
	rm -f "$STATE"
	log "uplink via $ROUTER dropped"
}

hook() {
	case "$1" in
	bound|renew)
		gw=${router%% *}
		[ -n "$ip" ] && [ -n "$gw" ] || exit 0
		addr="$ip/${mask:-24}"
		if [ -r "$STATE" ]; then
			. "$STATE"
			[ "$ADDR" = "$addr" ] && [ "$ROUTER" = "$gw" ] && exit 0
			drop
		fi
		ip addr replace "$addr" dev "$interface"
		# src: leave through the phone with the phone-given address, not 192.168.7.1
		ip route replace default via "$gw" dev "$interface" src "$ip" metric $METRIC
		[ -n "$dns" ] && resolvectl dns "$interface" $dns && resolvectl domain "$interface" '~.'
		printf 'IF=%s\nADDR=%s\nROUTER=%s\n' "$interface" "$addr" "$gw" > "$STATE"
		log "uplink via phone $gw ($interface $addr, dns $dns)"
		;;
	deconfig|nak)
		drop
		;;
	esac
	exit 0
}

run() {
	[ -r /run/usb-ncm.env ] && . /run/usb-ncm.env
	[ -n "$NCM_IF" ] || { echo "phone link (usb-ncm) is not up" >&2; exit 1; }
	drop
	# -f foreground, discover 2x3 s then pause 10 s: an idle probe until tethering is turned on.
	# Our own dnsmasq on the link does not answer: udhcpc sends through a raw socket.
	busybox udhcpc -f -i "$NCM_IF" -s "$SELF" -t 2 -T 3 -A 10 -x hostname:erl-node &
	pid=$!
	trap 'kill $pid 2>/dev/null; wait $pid; drop; exit 0' TERM INT

	misses=0
	while kill -0 $pid 2>/dev/null; do
		sleep $CHECK &
		wait $!
		[ -r "$STATE" ] || { misses=0; continue; }
		. "$STATE"
		# The phone keeps answering ARP while tethering is on. When it is turned off the lease
		# would still run for an hour: drop it now and go back to probing.
		if busybox arping -q -c 1 -w 2 -I "$IF" "$ROUTER"; then
			misses=0
		elif [ $((misses += 1)) -ge $MISSES ]; then
			log "phone $ROUTER stopped answering, Ethernet tethering off?"
			kill -USR2 $pid          # release: runs the hook with deconfig
			sleep 1
			drop
			kill -USR1 $pid          # and start discovering again
			misses=0
		fi
	done
	drop
	exit 1
}

case "$1" in
run) run ;;
bound|renew|deconfig|leasefail|nak) hook "$1" ;;
*) echo "usage: $0 run" >&2; exit 1 ;;
esac

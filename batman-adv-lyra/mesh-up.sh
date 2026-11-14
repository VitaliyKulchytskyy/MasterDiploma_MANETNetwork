#!/bin/sh
# Bring up a batman-adv mesh node over IBSS (ad-hoc) on the RTL8812AU dongle.
# Usage: mesh-up.sh <node-ip/prefix> [gw-mode]
#   mesh-up.sh 10.10.0.1/24          # plain node
#   mesh-up.sh 10.10.0.1/24 server   # node with internet uplink
#   mesh-up.sh 10.10.0.2/24 client   # node that should use mesh gateways
# Every node must use the same MESH_SSID, MESH_FREQ and MESH_BSSID.
# MESH_TXPOWER (dBm) optionally caps TX power, e.g. from /etc/default/mesh.
set -e

NODE_IP=${1:?usage: $0 <ip/prefix> [server|client|off]}
GW_MODE=${2:-off}

wait_if() {
	i=0
	while [ -z "$MESH_IF" ] && [ "$i" -lt "$1" ]; do
		MESH_IF=$(ls /sys/class/net | grep '^wlx' | head -n1)
		[ -n "$MESH_IF" ] || { i=$((i + 1)); sleep 1; }
	done
}

# At boot the USB dongle may enumerate after this runs; wait for it
wait_if 30

# Dongle present but no interface: the probe likely hit -ENOMEM (fragmented
# memory, order-5 alloc in mac80211). Compact memory and probe again.
if [ -z "$MESH_IF" ] && [ -d /sys/module/rtw_8812au ] && grep -qs 0bda /sys/bus/usb/devices/*/idVendor; then
	echo "no wlx interface, compacting memory and reloading rtw_8812au" >&2
	sync
	echo 3 > /proc/sys/vm/drop_caches
	echo 1 > /proc/sys/vm/compact_memory
	modprobe -r rtw_8812au
	modprobe rtw_8812au
	wait_if 15
fi
MESH_SSID=${MESH_SSID:-LyraMesh}
MESH_FREQ=${MESH_FREQ:-2437}               # 2.4 GHz channel 6
MESH_BSSID=${MESH_BSSID:-02:ca:fe:ca:ca:40} # fixed cell id, avoids IBSS splits
MESH_TXPOWER=${MESH_TXPOWER:-}              # optional TX cap in dBm (e.g. 10)
MESH_OGM_MS=${MESH_OGM_MS:-250}            # batman-adv announcement interval (default 1000): faster link/route updates
MESH_COUNTRY=${MESH_COUNTRY:-PA}           # regulatory rules: PA allows 2.4 GHz 1-13 and 5 GHz 36-165 without DFS (see mesh-radio)

[ -n "$MESH_IF" ] || { echo "no wlx* interface found" >&2; exit 1; }

# rtw88 ignores "iw set txpower"; its SAR limit (1/4 dBm units, bands 0-3) is
# the only cap it honours. Lower TX power cuts the dongle's current spike, which
# can brown out a weak USB supply the moment the mesh starts transmitting.
if [ -n "$MESH_TXPOWER" ]; then
	q=$((MESH_TXPOWER * 4))
	phy=$(cat "/sys/class/net/$MESH_IF/phy80211/name")
	iw phy "$phy" set sar_specs 0 0:$q 1:$q 2:$q 3:$q && echo "TX power capped at $MESH_TXPOWER dBm"
fi

# the country before the join: its rules decide which channels may start the ad-hoc cell
# (applied asynchronously by the kernel)
iw reg set "$MESH_COUNTRY" 2>/dev/null
i=0
while ! iw reg get 2>/dev/null | grep -q "^country $MESH_COUNTRY:" && [ "$i" -lt 10 ]; do
	i=$((i + 1))
	sleep 0.5
done

modprobe batman-adv

# Keep NetworkManager away from the mesh radio
nmcli dev set "$MESH_IF" managed no 2>/dev/null || true

# Unmanaging releases the interface from wpa_supplicant asynchronously, and its
# nl80211 deinit resets the iftype to managed; wait for its control socket to go
i=0
while [ -e "/run/wpa_supplicant/$MESH_IF" ] && [ "$i" -lt 10 ]; do
	i=$((i + 1))
	sleep 1
done
sleep 1

# Retry the radio setup in case something still flips the iftype back
i=0
while :; do
	ip link set "$MESH_IF" down
	iw dev "$MESH_IF" set type ibss 2>/dev/null
	# batman-adv adds a 32-byte header; raise MTU so bat0 can keep 1500
	ip link set "$MESH_IF" mtu 1532 || echo "warning: MTU 1532 not accepted, bat0 will fragment"
	ip link set "$MESH_IF" up
	if iw dev "$MESH_IF" ibss join "$MESH_SSID" "$MESH_FREQ" HT20 fixed-freq "$MESH_BSSID" 2>/dev/null &&
	   iw dev "$MESH_IF" info | grep -q "type IBSS"; then
		break
	fi
	i=$((i + 1))
	[ "$i" -lt 5 ] || { echo "failed to join IBSS on $MESH_IF" >&2; exit 1; }
	sleep 1
done

batctl if add "$MESH_IF"
ip link set bat0 up
ip addr replace "$NODE_IP" dev bat0
batctl gw_mode "$GW_MODE"
batctl it "$MESH_OGM_MS"

echo "mesh up: $MESH_IF -> bat0 $NODE_IP (gw_mode $GW_MODE)"

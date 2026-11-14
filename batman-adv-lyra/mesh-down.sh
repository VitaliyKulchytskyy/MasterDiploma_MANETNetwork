#!/bin/sh
# Tear down the batman-adv mesh and hand the dongle back to NetworkManager.
MESH_IF=${MESH_IF:-$(ls /sys/class/net | grep '^wlx' | head -n1)}

ip link del bat0 2>/dev/null

# Dongle unplugged or failed to probe: nothing left to reset
if [ -z "$MESH_IF" ]; then
	echo "mesh down: no wlx interface (dongle missing)"
	exit 0
fi

batctl if del "$MESH_IF" 2>/dev/null
iw dev "$MESH_IF" ibss leave 2>/dev/null
ip link set "$MESH_IF" down
iw dev "$MESH_IF" set type managed
ip link set "$MESH_IF" mtu 1500
ip link set "$MESH_IF" up
nmcli dev set "$MESH_IF" managed yes 2>/dev/null
echo "mesh down: $MESH_IF back to managed"

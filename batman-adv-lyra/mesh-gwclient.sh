#!/bin/sh
# udhcpc hook for mesh nodes with MESH_GW=client (mesh-gwclient.service).
# batman-adv (gw_mode client) delivers the DHCP request to the best mesh gateway; from its
# answer only the default route and DNS are taken, the node keeps its static mesh IP.
# A route of the node's own (e.g. a tethered phone, metric 100) still wins over this one.
METRIC=600

case "$1" in
bound|renew)
	gw=${router%% *}
	[ -n "$gw" ] || exit 0
	ip route replace default via "$gw" dev bat0 metric $METRIC
	[ -n "$dns" ] && resolvectl dns bat0 $dns && resolvectl domain bat0 '~.'
	logger -t mesh-gwclient "gateway $gw, dns $dns"
	;;
deconfig|leasefail|nak)
	ip route del default dev bat0 2>/dev/null
	resolvectl revert bat0 2>/dev/null
	;;
esac
exit 0

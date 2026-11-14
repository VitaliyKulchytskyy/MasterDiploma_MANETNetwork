#!/bin/sh
# apt packages an ERL node needs on the Luckfox Lyra Ubuntu 22.04 (armhf) image, so a fresh
# image can be brought up to what the deployed boards run. Runs on the board (the justfile
# pushes it: just packages / packages-bundle / packages-offline).
#
# Usage: board-packages check              what is missing
#        board-packages install [--dev]    apt-get install (the board needs internet)
#        board-packages bundle DIR [--dev] download every .deb needed (on a board WITH internet)
#        board-packages offline DIR        install from such a bundle (no internet needed)
#
# Everything comes from the Ubuntu ports archive (jammy, jammy-updates, universe), the
# sources of the stock image. Installed with --no-install-recommends: network-manager would
# otherwise pull in ModemManager, ppp, pptp and polkit, which a node does not use.
# Not installed on purpose: linux-firmware (1.1 GB; the RTL8812AU firmware comes with
# `just driver-deploy`) and the full dnsmasq package (its own service would take port 53;
# the scripts run /usr/sbin/dnsmasq from dnsmasq-base).
set -e

# Mesh radio: batman-adv over IBSS (mesh-up)
MESH="batctl iw wpasupplicant wireless-regdb"
# Phone link, mesh gateway, uplinks: DHCP/DNS (dnsmasq), udhcpc hooks, NAT (erl kernel),
# Wi-Fi / USB-tethering uplinks, the web UI (busybox httpd)
NET="dnsmasq-base busybox udhcpc iptables network-manager iproute2 iputils-ping"
# Services and tools the scripts rely on. Usually already in the image; listed so a slimmer
# image still works. systemd provides resolvectl, systemd-timesyncd sets the clock at boot
# (no RTC), libsqlite3-0 is merld's chat store.
SYSTEM="systemd systemd-timesyncd udev kmod procps mawk bsdutils libsqlite3-0"
# Debugging on the board only (--dev)
DEV="usbutils i2c-tools sqlite3 htop less ethtool git"

packages() {
	echo "$MESH $NET $SYSTEM"
	[ "$1" = --dev ] && echo "$DEV"
	return 0
}

missing() {
	for p in $(packages "$1"); do
		dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "ok installed" || echo "$p"
	done
}

apt_get() {
	DEBIAN_FRONTEND=noninteractive apt-get -y -o Dpkg::Options::=--force-confold --no-install-recommends "$@"
}

case "$1" in
check)
	for p in $(packages --dev); do
		v=MISSING
		dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "ok installed" &&
			v=$(dpkg-query -W -f='${Version}' "$p")
		case " $DEV " in *" $p "*) v="$v (dev)" ;; esac
		printf '%-20s %s\n' "$p" "$v"
	done
	[ -z "$(missing)" ] && echo "all required packages are installed" || { echo "missing: $(missing | tr '\n' ' ')"; exit 1; }
	;;
install)
	apt-get update
	apt_get install $(packages "$2")
	echo "installed: $(packages "$2" | wc -w) packages"
	;;
bundle)
	dir=${2:?usage: board-packages bundle DIR [--dev]}
	mkdir -p "$dir"
	apt-get update
	# The full dependency closure, so the bundle installs on an image that has none of them.
	# Virtual packages have no .deb and are skipped.
	closure=$(apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts \
		--no-breaks --no-replaces --no-enhances $(packages "$3") |
		grep -v '^ ' | grep -v '^<' | sort -u)
	cd "$dir"
	for p in $closure; do
		apt-cache show "$p" >/dev/null 2>&1 && apt-get download "$p" >/dev/null 2>&1 || true
	done
	echo "bundle: $(ls *.deb | wc -l) debs, $(du -sh . | cut -f1) in $dir"
	;;
offline)
	dir=$(realpath "${2:?usage: board-packages offline DIR}")
	# Only packages the image does not have: handing apt a bundled .deb older than the
	# installed one would ask for a downgrade. apt resolves the order from the local files.
	debs=""
	for f in "$dir"/*.deb; do
		p=$(dpkg-deb -f "$f" Package)
		dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "ok installed" || debs="$debs $f"
	done
	[ -n "$debs" ] && apt_get install $debs
	echo "installed from $dir:$(echo "$debs" | wc -w) packages"
	;;
*)
	sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
	;;
esac

#!/bin/sh
# One Wi-Fi sweep over every channel the radio may use, for the Node page's spectrum waterfall.
# The chip (RTL8812AU, rtw88) has no spectral scan, so a sweep is a Wi-Fi scan: every network
# heard, with its channel and signal. The radio leaves the mesh channel for the ~3 s of the scan.
#   wifi-scan            scan now; print the sweep as JSON and keep it in the history
#   wifi-scan history    the kept sweeps (the last KEEP), oldest first, as a JSON array
HIST=/run/erl/wifi-scan.jsonl
LOCK=/run/erl/wifi-scan.lock
KEEP=25         # = the rows the Node page draws

mkdir -p /run/erl

if [ "$1" = history ]; then
	printf '['
	[ -r "$HIST" ] && paste -sd, "$HIST"
	printf ']\n'
	exit 0
fi

IF=$(batctl if 2>/dev/null | head -n1 | cut -d: -f1)
[ -n "$IF" ] || IF=$(ls /sys/class/net | grep '^wlx' | head -n1)
[ -n "$IF" ] || { echo '{"ok":false,"error":"no Wi-Fi radio"}'; exit 1; }

# one scan at a time (two viewers of the page, or a scan still running)
exec 9> "$LOCK"
flock -w 15 9 || { echo '{"ok":false,"error":"another scan is still running"}'; exit 1; }

start=$(date +%s%N)
raw=$(iw dev "$IF" scan 2>&1) || { printf '{"ok":false,"error":"scan failed: %s"}\n' "$(echo "$raw" | head -n1 | tr -d '"\\')"; exit 1; }
ms=$((($(date +%s%N) - start) / 1000000))

# BSS lines -> {"bssid","freq","signal","ssid","ibss"}. iw prints SSID bytes outside ASCII as
# \xNN (a Cyrillic name is \xd0\x92...): backslashes and quotes are escaped for JSON first (sed:
# mawk's gsub does not double a backslash), the page turns \xNN back into UTF-8 text
bss=$(printf '%s\n' "$raw" | sed 's/\\/\\\\/g; s/"/\\"/g' | awk '
	function flush() {
		if (bssid != "" && freq != "")
			printf "%s{\"bssid\":\"%s\",\"freq\":%d,\"signal\":%s,\"ssid\":\"%s\",\"ibss\":%s}", sep, bssid, freq, (sig == "" ? "null" : sig), ssid, ibss ? "true" : "false"
		if (bssid != "" && freq != "") sep = ","
		bssid = ""; freq = ""; sig = ""; ssid = ""; ibss = 0
	}
	/^BSS / { flush(); bssid = substr($2, 1, 17); next }
	/^\tfreq:/ { freq = int($2); next }
	/^\tsignal:/ { sig = $2 + 0; next }
	/^\tSSID:/ {
		s = substr($0, index($0, "SSID:") + 6)
		gsub(/[\001-\037]/, "", s)
		ssid = s; next
	}
	/^\tcapability:/ { if ($0 ~ /IBSS/) ibss = 1; next }
	END { flush() }')

sweep=$(printf '{"t":%s,"ms":%s,"bss":[%s]}' "$(date +%s)" "$ms" "$bss")
printf '%s\n' "$sweep" >> "$HIST"     # not echo: dash's echo would undo the \\ escapes
tail -n $KEEP "$HIST" > "$HIST.tmp" && mv "$HIST.tmp" "$HIST"
printf '{"ok":true,"sweep":%s}\n' "$sweep"

# ERL diploma: one place to deploy / update everything. Drives the subprojects' justfiles:
#   batman-adv-lyra       mesh, gateway, phone link, web UI, Wi-Fi driver, apt packages (Lyra)
#   luckfoxLyra_mainPart  merld + merlctl + ina226d, chat database (Lyra)
#   erl-android           the phone app
#   emergencyRadioLink    RP2040 firmware
#
# Boards are the Lyra main units on adb (told apart from phones by their Ubuntu). With one
# board connected nothing needs to be named; with more, pass serial=<id> or use the *-all
# recipes. Each board's kernel (stock / erl) is read from the board itself.
#
#   just boards                 connected boards: kernel, mesh address, gateway role
#   just update                 everything on the board (drivers, services, web, merld)
#   just update-all             the same on every connected board
#   just web                    only the web UI (quick)
#   just setup 10.10.0.3/24 client    a fresh image: packages, everything, mesh + phone link
#   just app                    build + install the phone app
#   just app-all                the same on every connected phone
#   just firmware               build + flash the RP2040 firmware

set shell := ["bash", "-euo", "pipefail", "-c"]

# adb serial of the board (empty: the only board connected); phone= for the app
serial := ""
phone := ""

MESH := "batman-adv-lyra"
MAIN := "luckfoxLyra_mainPart"
APP := "erl-android"
FW := "emergencyRadioLink"
APK := "app/build/outputs/apk/debug/app-debug.apk"

default:
    @just --list --unsorted

# Connected Lyra boards: serial, kernel, mesh address, gateway role, UART link to the unit
boards:
    #!/usr/bin/env bash
    set -euo pipefail
    found=0
    for s in $(just _boards); do
        found=1
        adb -s "$s" shell '. /etc/default/mesh 2>/dev/null; printf "%-18s kernel %-12s mesh %-15s gw %-7s unit %s\n" \
            "'"$s"'" "$(uname -r)" "${MESH_IP:-none}" "${MESH_GW:-off}" \
            "$(merlctl status 2>/dev/null | sed -n "s/.*\"link\":\"\([a-z-]*\)\".*/\1/p")"'
    done
    [ $found = 1 ] || echo "no Lyra board on adb"

# Update everything on one board: Wi-Fi driver, batman-adv, mesh/gateway/phone-link services, web UI, merld, ina226d
update:
    #!/usr/bin/env bash
    set -euo pipefail
    s=$(just serial={{serial}} _serial)
    just _update "$s"

# Update every connected board, one after another
update-all:
    #!/usr/bin/env bash
    set -euo pipefail
    boards=$(just _boards)
    [ -n "$boards" ] || { echo "no Lyra board on adb" >&2; exit 1; }
    for s in $boards; do
        just _update "$s"
    done
    just boards

# Only the web UI (pages + CGIs), no rebuild
web:
    #!/usr/bin/env bash
    set -euo pipefail
    s=$(just serial={{serial}} _serial)
    cd {{MESH}} && just serial="$s" web-deploy
    
# Update web UI (pages + CGIs) on every connected board, one after another
web-all:
    #!/usr/bin/env bash
    set -euo pipefail
    boards=$(just _boards)
    [ -n "$boards" ] || { echo "no Lyra board on adb" >&2; exit 1; }
    for s in $boards; do
        just _web "$s"
    done
    just boards

# Only merld / merlctl / ina226d
main:
    #!/usr/bin/env bash
    set -euo pipefail
    s=$(just serial={{serial}} _serial)
    cd {{MAIN}} && just serial="$s" deploy

# Bring a fresh Ubuntu image up: apt packages, everything, mesh at boot, phone link. gw: server|client|off
setup ip gw="client" txpower="":
    #!/usr/bin/env bash
    set -euo pipefail
    s=$(just serial={{serial}} _serial)
    (cd {{MESH}} && just serial="$s" packages)
    just _update "$s"
    cd {{MESH}}
    just serial="$s" mesh-enable {{ip}} {{gw}} {{txpower}}
    just serial="$s" phone-enable
    adb -s "$s" shell 'systemctl restart mesh.service; sync'
    cd .. && just boards

# Service state, mesh neighbours and the emergency unit on one board
status:
    #!/usr/bin/env bash
    set -euo pipefail
    s=$(just serial={{serial}} _serial)
    adb -s "$s" shell 'for u in mesh mesh-gw mesh-gw-dns mesh-gwclient erl-inet usb-ncm usb-ncm-dhcp usb-uplink erl-web merld ina226d; do
            printf "%-14s %-9s %s\n" $u "$(systemctl is-enabled $u.service 2>/dev/null)" "$(systemctl is-active $u.service)"; done
        echo "--- mesh"; batctl n 2>/dev/null | tail -n +3; batctl gwl 2>/dev/null | tail -n +2
        echo "--- internet"; cat /run/erl/internet.json 2>/dev/null
        echo "--- emergency unit"; merlctl status 2>/dev/null | head -c 400; echo'

# Copy every connected board's chat history to luckfoxLyra_mainPart/backups
backup-all:
    #!/usr/bin/env bash
    set -euo pipefail
    for s in $(just _boards); do
        (cd {{MAIN}} && just serial="$s" db-backup)
    done

# Build and install the phone app (phone=<adb id> with several devices, e.g. wireless debugging ip:port)
app:
    cd {{APP}} && just serial={{phone}} install

# Build the app once and install it on every connected Android phone (boards and emulators left out)
app-all:
    #!/usr/bin/env bash
    set -euo pipefail
    phones=$(just _phones)
    [ -n "$phones" ] || { echo "no phone on adb (USB debugging, or adb pair/connect for wireless)" >&2; exit 1; }
    # one build, so every phone gets the same build number
    (cd {{APP}} && just build)
    for p in $phones; do
        echo "=== $p: $(adb -s "$p" shell getprop ro.product.model | tr -d '\r')"
        adb -s "$p" install -r {{APP}}/{{APK}}
    done

# Build and flash the RP2040 emergency unit firmware (preset: see emergencyRadioLink/CMakePresets.json)
firmware preset="":
    cd {{FW}} && just {{ if preset == "" { "flash" } else { "flash " + preset } }}

# USB Ethernet from this PC to a plugged-in node: just pc-link up [--share] | down | status [-s SERIAL]
pc-link +args:
    ./pc-link.sh {{args}}

# Host tests: the MERL library
test:
    cd {{MAIN}} && just test-lib

# ---- helpers ------------------------------------------------------------------------------

# Serials of the Lyra boards on adb (phones and emulators left out)
[private]
_boards:
    #!/usr/bin/env bash
    for s in $(adb devices | awk 'NR > 1 && $2 == "device" {print $1}'); do
        timeout 10 adb -s "$s" shell 'grep -q "^NAME=\"Ubuntu\"" /etc/os-release 2>/dev/null && echo yes' 2>/dev/null |
            grep -q yes && echo "$s"
    done
    true

# Serials of the Android phones on adb (answer getprop with an SDK level; emulators left out)
[private]
_phones:
    #!/usr/bin/env bash
    for s in $(adb devices | awk 'NR > 1 && $2 == "device" && $1 !~ /^emulator-/ {print $1}'); do
        sdk=$(timeout 10 adb -s "$s" shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r')
        # a number: an error text from a board's shell ("getprop: not found") is not a phone
        [[ "$sdk" =~ ^[0-9]+$ ]] && echo "$s"
    done
    true

# The board to work on: serial=, or the only one connected
[private]
_serial:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -n "{{serial}}" ]; then echo "{{serial}}"; exit 0; fi
    boards=$(just _boards)
    case $(echo "$boards" | grep -c .) in
    1) echo "$boards" ;;
    0) echo "no Lyra board on adb" >&2; exit 1 ;;
    *) echo "several boards on adb, pass serial=<id> (or use update-all):" >&2; echo "$boards" >&2; exit 1 ;;
    esac

# Everything for one board; the modules are built for the kernel the board runs
[private]
_update s:
    #!/usr/bin/env bash
    set -euo pipefail
    s="{{s}}"
    kver=$(adb -s "$s" shell uname -r | tr -d '\r')
    case "$kver" in *-erl) kernel=erl ;; *) kernel=stock ;; esac
    echo "=== $s: kernel $kver -> modules for $kernel"
    (cd {{MESH}} && just serial="$s" kernel=$kernel driver-deploy deploy)
    (cd {{MAIN}} && just serial="$s" deploy)
    adb -s "$s" shell sync
    echo "=== $s updated (Wi-Fi driver changes apply after a dongle replug or reboot)"
    
# Update web UI (pages + CGIs) on one board
[private]
_web s:
    #!/usr/bin/env bash
    set -euo pipefail
    s="{{s}}"
    echo "=== $s: updating web UI"
    (cd {{MESH}} && just serial="$s" web-deploy)
    adb -s "$s" shell sync
    echo "=== $s web UI updated"
   

#!/bin/sh
# the scenario: fake unit + merld + web server on :8000, chat history filled
D=$(cd "$(dirname "$0")" && pwd); M=/home/karlenko/dev/diploma/luckfoxLyra_mainPart; R=/tmp/claude-1000/demo-run
rm -rf $R; mkdir -p $R
$M/build/host/merld -I -c $R/chat.db >/dev/null
python3 $D/chat_fill.py $R/chat.db history
setsid python3 $D/demo_unit.py $R/tty --peers 6 > $R/unit.log 2>&1 < /dev/null &
sleep 1
setsid $M/build/host/merld -d $R/tty -s $R/sock -o $R/state.json -c $R/chat.db -p 1000 > $R/merld.log 2>&1 < /dev/null &
setsid python3 $D/demo_web.py --sock $R/sock > $R/web.log 2>&1 < /dev/null &
sleep 4

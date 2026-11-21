#!/bin/sh
CONSOLE_USER=`/usr/bin/stat -f%Su /dev/console`
/usr/bin/sudo -u $CONSOLE_USER /usr/bin/killall MozkeyIbGConverter > /dev/null
/usr/bin/sudo -u $CONSOLE_USER /usr/bin/killall MozkeyIbGRenderer > /dev/null
/bin/ps -axo pid=,comm= | /usr/bin/awk '/\/Library\/Input Methods\/MozkeyIbG\.app\/.*\/mozc_zenz_scorer$/ { print $1 }' | /usr/bin/xargs /bin/kill > /dev/null 2>&1
/usr/bin/sudo -u $CONSOLE_USER /usr/bin/killall llama-server > /dev/null 2>&1
/usr/bin/sudo -u $CONSOLE_USER /usr/bin/killall MozkeyIbG > /dev/null
/usr/bin/true

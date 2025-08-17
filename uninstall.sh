#!/system/bin/sh
BASE=/data/local/touchmux
[ -f $BASE/pid ] && kill "$(cat $BASE/pid)" 2>/dev/null
rm -rf $BASE

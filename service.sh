#!/system/bin/sh
MODDIR=${0%/*}
BASE=/data/local/touchmux
BIN=$MODDIR/bin/touchmux

EV=$(
  getevent -pl 2>/dev/null | awk '
    /^add device/ {dev=$5}
    /name:.*(touch|touchscreen|goodix|novatek|synaptics|elan|fts)/I {
      gsub(/.*\/dev\/input\//,"",dev); print dev; exit
    }'
)
[ -z "$EV" ] && exit 0
DEV=/dev/input/$EV

$BIN --src "$DEV" --grab=1 --verbose=0 &
echo $! > $BASE/pid

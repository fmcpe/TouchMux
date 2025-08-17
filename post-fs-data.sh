#!/system/bin/sh
MODDIR=${0%/*}
BASE=/data/local/touchmux
mkdir -p $BASE
chown root:shell $BASE
chmod 0775 $BASE

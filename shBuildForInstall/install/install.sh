#!/bin/sh
export KSROOT=/koolshare
source $KSROOT/scripts/base.sh

cp -r /tmp/merlinc/* $KSROOT/
chmod a+x $KSROOT/scripts/merlinc_*

# add icon into softerware center
dbus set softcenter_module_merlinc_install=1
dbus set softcenter_module_merlinc_version=0.1
dbus set softcenter_module_merlinc_description="Merlin 實用工具"

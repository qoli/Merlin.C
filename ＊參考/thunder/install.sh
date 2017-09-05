#!/bin/sh

cp -r /tmp/thunder/* /koolshare/
chmod a+x /koolshare/scripts/*-xunlei.sh

# add icon into softerware center
dbus set softcenter_module_thunder_install=1
dbus set softcenter_module_thunder_version=0.1
dbus set softcenter_module_thunder_home_url=Module_xunlei.asp

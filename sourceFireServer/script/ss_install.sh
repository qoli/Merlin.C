#!/bin/sh

cd /tmp
wget https://raw.githubusercontent.com/koolshare/koolshare.github.io/acelan_softcenter_ui/shadowsocks/shadowsocks.tar.gz
tar -zxvf /tmp/shadowsocks.tar.gz
chmod +x /tmp/shadowsocks/install.sh 
sh /tmp/shadowsocks/install.sh
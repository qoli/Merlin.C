#!/bin/sh
date
echo ""
case $1 in
CC|cc)
	echo "CC: Cross-Compiler"
	echo ""
	cd lib/libevent-2.1.8-stable
	./configure -disable-shared -enable-static --prefix=/opt/crossinstall/libevent --host=arm-linux CC=arm-linux-gcc CXX=arm-linux-g++
	;;
*)
	echo "[SH] arm-linux-gcc"
	arm-linux-gcc fireServer.c -o ./Binary/fireServer -I/opt/crossinstall/libevent/include/ -L/opt/crossinstall/libevent/lib/ -lrt -levent -static
	arm-linux-gcc fireClientRead.c -o ./Binary/fireClientRead -I/opt/crossinstall/libevent/include/ -L/opt/crossinstall/libevent/lib/ -lrt -levent -static
	arm-linux-gcc fireClientSend.c -o ./Binary/fireClientSend -I/opt/crossinstall/libevent/include/ -L/opt/crossinstall/libevent/lib/ -lrt -levent -static
	echo ""
	echo "[SH] copy Binary to Router"
	scp ./Binary/fireServer admin@192.168.1.1:/tmp/home/root/MerlinC
	scp ./Binary/fireClientRead admin@192.168.1.1:/tmp/home/root/MerlinC
	scp ./Binary/fireClientSend admin@192.168.1.1:/tmp/home/root/MerlinC
	scp ./runServer.sh admin@192.168.1.1:/tmp/home/root/MerlinC
	;;
esac
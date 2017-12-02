#!/bin/sh
date
echo ""
echo "[SH] g++ Compiler"
gcc fireTestClient.c -o fireTestClient -levent
echo ""
echo "[RUN] fireTestClient"
echo ""
./fireTestClient 6088 1024 0 30

# python chat_client.py 192.168.1.1 6088
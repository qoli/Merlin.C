#!/bin/sh
date
echo ""
echo "[SH] g++ Compiler"
g++ fireTestClient.cpp -o fireTestClient -levent
echo ""
echo "[RUN] fireTestClient"
echo ""
./fireTestClient
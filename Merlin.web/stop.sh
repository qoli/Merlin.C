#!/bin/sh
pid=$(ps | grep boa-conf | awk 'NR==1{print $1}')
kill $pid

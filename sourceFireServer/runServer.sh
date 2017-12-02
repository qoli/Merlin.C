#!/bin/sh

slp="6s"

echo ""
echo "[SH] runServer"
netstat -anp | grep 6088
echo ""
while [ true ]; do
	code=$(netstat -anp | grep 6088 | awk '{print $6}')
	
	if [ -n "$code" ]; then
		echo 'CODE: '${code}''
		echo "wait ... ${slp}"
	    sleep $slp
	else
		break
	fi

done

./fireServer
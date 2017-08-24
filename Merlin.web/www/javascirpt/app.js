$(document).ready(function() {

	setInterval(function() {
		$("#ajax").load("http://192.168.1.1:82/cgi-bin/run.cgi?../script/netspeed.sh%20ppp0");
	}, 2500)

});

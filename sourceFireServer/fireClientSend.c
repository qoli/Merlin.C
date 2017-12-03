#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Libevent. */
#include <event2/event.h>

/* fireHeadres. */
#include "Headres/fireHeadres.h"

/* define. */
#define SERVER_PORT 6088
#define SERVER_ADDR "192.168.1.1"

void usage(int argc, char *argv0) {
	if (argc == 1) {
		fprintf(stderr, "Usage: %s Message\n", argv0);
		exit(0);
	}
}

int main(int argc, char **argv) {
	int sockfd;
	char str[128];

	usage(argc,argv[0]);

	// 鏈接服務器方法
	sockfd = tcp_connect_server(SERVER_ADDR, SERVER_PORT);

	// 拼接換行符號
	strcpy(str, "[FIRE]:");
	strcat(str, argv[1]);
	strcat(str, "\n");

	if ( send(sockfd , str , strlen(str) , 0) < 0) {
		puts("Send failed");
		return 1;
	}

	return 0;
}
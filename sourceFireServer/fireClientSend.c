#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>

/* Libevent. */
#include <event2/event.h>

#define BUFSIZE 128
#define SERVER_PORT 6088
#define SERVER_ADDR "192.168.1.1"


int tcp_connect_server(const char* server_ip, int port) {
	struct sockaddr_in server_addr;
	int status = -1;
	int sockfd;

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);
	status = inet_aton(server_ip, &server_addr.sin_addr);
	if (0 == status)
	{
		errno = EINVAL;
		return -1;
	}

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if ( sockfd == -1 )
		return sockfd;

	status = connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr) );

	if ( status == -1 )
	{
		close(sockfd);
		return -1;
	}

	evutil_make_socket_nonblocking(sockfd);

	return sockfd;
}

void usage(char *argv0) {
	fprintf(stderr,
	        "Usage: %s Message\n",
	        argv0);
}

int main(int argc, char **argv) {
	int sockfd;
	char str[128];

	if (argc == 1)
	{
		usage(argv[0]);
		return;
	}

	// 鏈接服務器方法
	sockfd = tcp_connect_server(SERVER_ADDR, SERVER_PORT);

	strcpy(str, argv[1]);
	strcat(str, "\n");

	if ( send(sockfd , str , strlen(str) , 0) < 0) {
		puts("Send failed");
		return 1;
	}

	return 0;
}
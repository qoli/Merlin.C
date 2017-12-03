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
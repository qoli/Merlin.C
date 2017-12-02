#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* Libevent. */
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/event.h>

#define SERVER_PORT 6088
#define SERVER_ADDR "192.168.1.1"

void doDoctor() {
	system("chmod +x ./script/*.sh");
}

void doExec() {
	char err[50] = "> not found";
	char command[128] = "";

	// strcat(command, "./script/");
	// strcat(command, client_message);

	// // send(client_sock , client_message , strlen(client_message), 0);
	// printf("[EXEC] Command: > %s\n", command);

	// char buf[BUFSIZE];
	// FILE *fp;

	// if ((fp = popen(command, "r")) == NULL) {
	// 	printf("[S] Error opening pipe!\n");
	// }

	// while (fgets(buf, BUFSIZE, fp) != NULL) {
	// 	// Do whatever you want here...
	// 	send(client_sock , buf , BUFSIZE, 0);
	// 	printf("%s", buf);
	// }

	// if (pclose(fp))  {
	// 	send(client_sock , err , BUFSIZE, 0);
	// 	printf("\n[S] Command not found or exited with error status\n");
	// }
}

/*事件处理回调函数*/
void event_cb(struct bufferevent* bev, short events, void* ptr) {
	if (events & BEV_EVENT_CONNECTED) //连接建立成功
	{
		printf("connected to server successed!");
	}
	else if (events & BEV_EVENT_ERROR)
	{
		printf("connect error happened!");
	}
}

void read_cb(struct bufferevent *bev, void *ctx) {

	char msg[8192] = {0};
	struct evbuffer* buf = bufferevent_get_input(bev);
	evbuffer_copyout(buf, (void*)msg, 8192);

	printf("%s", msg);
}

void cmd_msg_cb(int fd, short events, void *arg) {
	char msg[1024];
	int ret = read(fd, msg, sizeof(msg));
	if (ret < 0 )
	{
		perror("read failed");
		return;
	}

	struct bufferevent* bev = (struct bufferevent*)arg;

	msg[ret] = '\0';
	//把终端消息发给服务器段
	bufferevent_write(bev, msg, ret);

	printf("send message %s\r\n", msg);
}

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

int main(int argc, char **argv)
{

	printf("fireClientRead ... \n\n");

	struct event_base* base = NULL;
	struct bufferevent *bev = NULL;
	int sockfd;

	//申请event_base对象
	base = event_base_new();

	// 鏈接服務器方法
	sockfd = tcp_connect_server(SERVER_ADDR, SERVER_PORT);
	bev = bufferevent_socket_new(base, sockfd, BEV_OPT_CLOSE_ON_FREE);

	//监听终端的输入事件
	struct event* ev_cmd = event_new(base, STDIN_FILENO, EV_READ | EV_PERSIST, cmd_msg_cb, (void*)bev);

	//添加终端输入事件
	event_add(ev_cmd, NULL);

	//设置bufferevent各回调函数
	bufferevent_setcb(bev, read_cb, NULL, event_cb, (void*)NULL);

	//启用读取或者写入事件
	bufferevent_enable(bev, EV_READ | EV_PERSIST);

	//开始事件管理器循环
	event_base_dispatch(base);

	event_base_free(base);


	return 0;
}
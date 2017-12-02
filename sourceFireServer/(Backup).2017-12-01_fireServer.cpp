#include <stdio.h>
#include <string.h>    //strlen
#include <sys/socket.h>
#include <arpa/inet.h> //inet_addr
#include <unistd.h>    //write
#include <stdlib.h>

#define BUFSIZE 128

int doDoctor() {
	system("chmod +x ./script/*.sh");
	return 0;
}

int main(int argc , char *argv[])
{

	int socket_desc , client_sock , c , read_size;
	struct sockaddr_in server , client;
	char client_message[2000];

		//Create socket
	socket_desc = socket(AF_INET , SOCK_STREAM , 0);
	if (socket_desc == -1)
	{
		printf("[S] Could not create socket");
	}
	puts("[S] Socket created");

		//Prepare the sockaddr_in structure
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port = htons( 6088 );

		//Bind
	if( bind(socket_desc,(struct sockaddr *)&server , sizeof(server)) < 0)
	{
				//print the error message
		perror("[S] bind failed. Error");
		return 1;
	}
	puts("[S] bind done");

		//Listen
	listen(socket_desc , 3);

		//Accept and incoming connection
	puts("[S] Waiting for incoming connections...\n");
	c = sizeof(struct sockaddr_in);

	while(1) {
		//accept connection from an incoming client
		client_sock = accept(socket_desc, (struct sockaddr *)&client, (socklen_t*)&c);
		if (client_sock < 0)
		{
			perror("[S] accept failed");
			return 1;
		}
		
		fprintf(stdout,"[S] Conencted from %s\n", inet_ntoa(client.sin_addr)); //输出连接到服务端的IP地址
		puts("[S] Connection accepted");

		//Receive a message from client
		while( (read_size = recv(client_sock , client_message , BUFSIZE , 0)) > 0 )
		{
			//Send the message back to client

			printf("[IN] Message: > %s\n\n", client_message);

			char doctor[] = "doctor";

			if (strcmp(client_message,doctor)==0) {
				doDoctor();
				send(client_sock , "doDoctor..." , BUFSIZE, 0);
			} else {
				char err[50] = "> not found";
				char command[128] = "";

				strcat(command,"./script/");
				strcat(command,client_message);

				// send(client_sock , client_message , strlen(client_message), 0);
				printf("[EXEC] Command: > %s\n", command);

				char buf[BUFSIZE];
				FILE *fp;

				if ((fp = popen(command, "r")) == NULL) {
					printf("[S] Error opening pipe!\n");
				}

				while (fgets(buf, BUFSIZE, fp) != NULL) {
	        		// Do whatever you want here...
					send(client_sock , buf , BUFSIZE, 0);
					printf("%s", buf);
				}

				if(pclose(fp))  {
					send(client_sock , err , BUFSIZE, 0);
					printf("\n[S] Command not found or exited with error status\n");
				}
			}

			





		}

		if(read_size == 0)
		{
			puts("[S] Client disconnected");
			fflush(stdout);
		}
		else if(read_size == -1)
		{
			perror("[S] recv failed");
		}
	}
	return 0;
}

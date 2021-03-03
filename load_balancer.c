#include <stdio.h> 
#include <netdb.h> 
#include <netinet/in.h> 
#include <stdlib.h> 
#include <string.h> 
#include <unistd.h>
#include <sys/socket.h> 
#include <sys/types.h>
#include <arpa/inet.h>


void prepare_query(char *buff, int buff_len, long low, long high) {
	// sleep(1);
	if(high <= low) high = low + 1;
	long num = low + rand() % (high - low);
	int i = buff_len - 1;
	while(num > 0) {
		buff[i--] = num % 10 + '0';
		num /= 10;
	}
	while(i >= 0) buff[i--] = '\0';
	strncpy(buff, "prime", 5);
}

void print(char *buff, int len) {
	for(int i = 0; i < len; i++) printf("%c", buff[i]);
}
void println(char *buff, int buff_len) {
	print(buff, buff_len);
	printf("\n");
}

void query(int server_fd) {
	
	static int buff_len = 25;
	char buff[25];
	printf("Query: ");
	while(1) {
		bzero(buff, sizeof(buff));
		strncpy(buff, "prime", 5);
		prepare_query(buff, buff_len, 0, 50);
		println(buff, buff_len);

		write(server_fd, buff, sizeof(buff));
		if(strncmp(buff, "exit", 4) == 0) break;

		bzero(buff, sizeof(buff));
		read(server_fd, buff, sizeof(buff));
		printf("Server: ");
		println(buff, buff_len);
		printf("Query: ");
	}
	printf("Client exit\n");
}

// create load balancer emthod and create thread for each server fd and call query method for each server fd. and also when server stops make that thread break the query method and close server fd.
// when server starts(monitor notifies) then create new thread(i.e call query method for thread).
// main thread will communicate with monitor.

void main() {
	struct sockaddr_in server_address;

	int sock_fd, flag;
	sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(sock_fd == -1) {
		printf("socket creation failed");
		exit(0);
	} else printf("socket created\n");

	server_address.sin_family = AF_INET;
	// IP of server pc to connect with. INADDR_LOOPBACK is 127.0.0.1 i.e. localhost you can specify IP
	server_address.sin_addr.s_addr =  inet_addr("192.168.122.89"); //htonl(INADDR_ANY);
	// port of server process on server pc.
	server_address.sin_port = htons(8080);

	flag = connect(sock_fd, (struct sockaddr *)&server_address, sizeof(server_address));
	if(flag == -1) {
		printf("Error conecting\n");
		exit(0);
	} else printf("connected\n");

	query(sock_fd);
	close(sock_fd);
}

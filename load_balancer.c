/*
This server program echoes the client query. Multiple clients are supported.
Number of worder threads can be specified for handling client query. Main process is
handling listening socket in main() mehtod.

Each worker thread is handling one epoll instance in this design(single worker thread can handle many epoll instances), one epoll instance is handling many socket fds for events.
new client's connection request is allocated to worker threads in round robbin fashion.

*/

#include <stdio.h> 
#include <stdlib.h> 
#include <string.h> 
#include <stdbool.h>
#include <sys/socket.h> 
#include <sys/types.h> 
#include <netdb.h> 
#include <netinet/in.h> 
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <arpa/inet.h>


#define SUCCESS 1
#define FAILED -1 // don't change to zero could be treated as socket_fd in connect_to_server() method.

// function prototypes
void *process_server_responses(void *arg); // server method to echo the client query. we can prepare server response for query.
void init_response_thread(); // creating threads and creating epoll instance for each thread.
void make_non_block_socket(int fd); // make the socket fd non blocking so that read/write on fd can be performed without blocking.
int create_lstn_sock_fd(); // create listening socket.


struct my_epoll_context { // this is custom structure used for data storation.
	int epoll_fd; // this is file descriptor of epoll instance. we will add remove socket fds using this epoll_fd.
	struct epoll_event *response_events; // when we wait on epoll then list of event will be returned(of type 'struct epoll_event') and we will store those in this memory (NOTE: we have already created memory for this pointer).
	// you can store response events anywhere but it is good to store the data related to same epoll in same structure.
} my_epoll; // only one epoll is created to receive responses from server. and any no of threads can manage it but I used one thread to monitor responses.

struct live_server_entry {
	char *IP;
	int server_sock_fd;
	struct live_server_entry* next;
} *live_serv_list = NULL;

struct live_server_entry* insert_server_entry(char *IP, int server_sock_fd) {
	struct live_server_entry* eptr = malloc(sizeof(struct live_server_entry));
	eptr->IP = calloc(strlen(IP)+1, sizeof(char));
	strcpy(eptr->IP, IP);
	eptr->server_sock_fd = server_sock_fd;
	eptr->next = NULL;

	if(live_serv_list == NULL) {
		live_serv_list = eptr;
		return eptr;
	}
	eptr->next = live_serv_list;
	live_serv_list = eptr;
	return eptr;
}

void delete_server_entry(char *IP) {
	if(live_serv_list == NULL) {
		return;
	}
	struct live_server_entry* eptr = live_serv_list;
	if(strcmp(eptr->IP, IP) == 0) {
		live_serv_list = eptr->next;
		free(eptr);
		return;
	}
	while(eptr->next != NULL && strcmp(eptr->next->IP, IP) != 0) {
		eptr = eptr->next;
	}
	if(eptr->next == NULL) return;
	struct live_server_entry* tmp = eptr->next;
	eptr->next = eptr->next->next;
	free(tmp);
	return;
}

struct live_server_entry* get_server_entry(char *IP) {
	if(live_serv_list == NULL) {
		return NULL;
	}
	struct live_server_entry* eptr = live_serv_list;
	while(eptr != NULL && strcmp(eptr->IP, IP) != 0) {
		eptr = eptr->next;
	}
	return eptr;
}

void *generate_requests(void *arg) {
	// TODO:
	// use system time and for given interval generate fixed number of request in round-robbin so that when new domain is spawns then no of request per domain decreases.
}

void init_request_thread() {
	pthread_t req_thread; // request generator threads.
	pthread_create(&req_thread, NULL, &generate_requests, (void *)-1); // creating the thread
}


void *process_server_responses(void *arg) {

	static int buff_len = 25;
	char buff[buff_len];
	int nfds, len;
	while(true) {
		nfds = epoll_wait(my_epoll.epoll_fd, my_epoll.response_events, 10, -1);// 10 is the maxevents to be returned by call (we have allocated space for 10 events during epoll instance creation you can increase) -1 timeout means it will never timeout means call returns in case of events/interrupts.
		for(int i = 0; i < nfds; i++) {
			int sock_fd = my_epoll.response_events[i].data.fd;

			len = read(sock_fd, buff, sizeof(buff));
			if(len == 0) { // event occured but client didn't query means client disconnected. don't close fd let the autoscaler inform what to do.
				continue;
			}
			/*
			TODO: read until socket is empty.
			*/


			// TODO: make buff length 50 and print request with response don't let server modify the request sent.
			
			printf("Server response: %s\n", buff);
		}
	}
}

void init_response_thread() {
	pthread_t res_thread; // response threads.
	my_epoll.epoll_fd = epoll_create1(0); // creating the epoll instance for this thread it returns the epoll instance fd.
	my_epoll.response_events = calloc(10, sizeof(struct epoll_event)); // this memory location will be passed to epoll_wait to write the response events.
	pthread_create(&res_thread, NULL, &process_server_responses, (void *)-1); // creating the thread
}

void make_non_block_socket(int fd) {
	int flags = fcntl(fd, F_GETFL, 0); // getting current flags of socket. F_GETFL is get flag command.
	flags |= O_NONBLOCK; // adding one more flag to socket. F_SETFL is set flag command.
	flags = fcntl(fd, F_SETFL, flags); // setting the new flag.
	if(flags == -1) {
		printf("non block failed for fd: %d", fd);
		exit(0);
	}
}

int create_lstn_sock_fd() {
	int flag;
	
	struct sockaddr_in lstn_socket, client_addr;
	
	int lstn_sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(lstn_sock_fd == -1) {
		printf("listening socket creation failed\n");
		exit(0);
	} else printf("listening socket created\n");

	lstn_socket.sin_family = AF_INET;
	lstn_socket.sin_addr.s_addr = htonl(INADDR_ANY); // since autoscaler is on same host.
	lstn_socket.sin_port = htons(8181); // auto scaler will connect on this port.

	flag = bind(lstn_sock_fd, (struct sockaddr *)&lstn_socket, sizeof(lstn_socket));
	if(flag == -1) {
		printf("Bind failed\n");
		exit(0);
	} else printf("Bind successful\n");

	flag = listen(lstn_sock_fd, 5);
	if(flag == -1) {
		printf("Error listening on socket\n");
		exit(0);
	} else printf("listening...\n");
	return lstn_sock_fd;
}


int connect_to_server(char *IP) {
	struct sockaddr_in server_address;

	int sock_fd, flag;
	sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(sock_fd == -1) {
		printf("server socket creation failed");
		return FAILED;
	} else printf("server socket created\n");

	server_address.sin_family = AF_INET;
	// IP of server pc to connect with. INADDR_LOOPBACK is 127.0.0.1 i.e. localhost you can specify IP
	server_address.sin_addr.s_addr =  inet_addr(IP); //htonl(INADDR_ANY);
	// port of server process on server pc.
	server_address.sin_port = htons(8080);

	flag = connect(sock_fd, (struct sockaddr *)&server_address, sizeof(server_address));
	if(flag == -1) {
		printf("Error conecting server at IP: %s\n", IP);
		return FAILED;
	} else printf("Connected to server at IP: %s\n", IP);
	return sock_fd;
}


int main() {
	int auto_sclr_sock_fd, len, flag, turn = 0;
	int lstn_sock_fd; // listening socket fd used to connect to auto scaler.

	lstn_sock_fd = create_lstn_sock_fd();

	struct sockaddr_in client_addr;
	len = sizeof(client_addr);
	// accept(listen_fd, NULL, NULL); we can use this also because we are not using client IP,port etc. hence no point in passing second argument. second argument is reference to structure in which connected clients IP, port is stored.
	while(true) {
		auto_sclr_sock_fd = accept(lstn_sock_fd, (struct sockaddr *)&client_addr, &len);
		if(auto_sclr_sock_fd == -1) {
			fprintf(stderr, "Error connecting autoscaler\n");
			sleep(3);
			continue;
		}
		printf("connected to autoscaler\n");
		break;
	}

	// for EPOLLET events it is advisable to use non-blocking operations on fd eg. read/write on socket.
	make_non_block_socket(auto_sclr_sock_fd);

	init_request_thread();

	init_response_thread(); // response collector thread.

	static int msg_len = 50;
	char message[msg_len];
	char *TYPE;
	char *IP;
	char *STR_SUCCESS = "SUCCESS";
	char *STR_FAILED = "FAILED";
	while(true) { // talk to autoscaler.
		int flag = read(auto_sclr_sock_fd, message, msg_len);
		if(flag == 0) {
			sleep(1);
			continue; // no message;
		}

		if(flag != msg_len) {
			fprintf(stderr, "Error reading message\n");
			continue;
		}
		TYPE = strtok(message, ";");
		IP = strtok(NULL, ";");
		struct live_server_entry* ptr = get_server_entry(IP);

		if(strcmp(TYPE, "SCALE_OUT") == 0) {
			if(ptr != NULL) { 	// already running.
				strcpy(message, STR_SUCCESS);
				write(auto_sclr_sock_fd, message, msg_len);
				continue;
			}
			int server_sock_fd = connect_to_server(IP);
			if(server_sock_fd < 0) {
				strcpy(message, STR_FAILED);
				write(auto_sclr_sock_fd, message, msg_len);
				continue;
			}
			make_non_block_socket(server_sock_fd); // so that response thread do not block(means entire process does not block)

			struct epoll_event interested_event; // struct epoll_event is inbuilt structure we just created variable of this struct type to store interested event data for this epoll instance.
			interested_event.data.fd = server_sock_fd; // adding the socket fd
			interested_event.events = EPOLLIN | EPOLLET; // adding the event type for this socket fd.
			epoll_ctl(my_epoll.epoll_fd, EPOLL_CTL_ADD, server_sock_fd, &interested_event); // adding the socket to epoll instance.
			printf("socket fd:%d added to epoll\n", server_sock_fd);
			continue;
		}
		if(strcmp(TYPE, "SCALE_IN") == 0) {
			if(ptr == NULL) {
				strcpy(message, STR_SUCCESS);
				write(auto_sclr_sock_fd, message, msg_len);
				continue;
			}
			// 
			if(close(ptr->server_sock_fd) == 0) { // since only of sock_fd for each IP(no multiple fds by using dup, dup2) hence closing fd will also remove from epoll context no need of epoll_ctl(EPOLL_CTL_DEL)
				strcpy(message, STR_SUCCESS);
				write(auto_sclr_sock_fd, message, msg_len);
				delete_server_entry(IP);
				continue;
			}
			strcpy(message, STR_FAILED);
			write(auto_sclr_sock_fd, message, msg_len);
			continue;
		}
	}

	close(lstn_sock_fd);
	close(auto_sclr_sock_fd);
	// close these fds in singnal handler because this is unreachable.
}
















































// void prepare_query(char *buff, int buff_len, long low, long high) {
// 	// sleep(1);
// 	if(high <= low) high = low + 1;
// 	long num = low + rand() % (high - low);
// 	int i = buff_len - 1;
// 	while(num > 0) {
// 		buff[i--] = num % 10 + '0';
// 		num /= 10;
// 	}
// 	while(i >= 0) buff[i--] = '\0';
// 	strncpy(buff, "prime", 5);
// }

// void print(char *buff, int len) {
// 	for(int i = 0; i < len; i++) printf("%c", buff[i]);
// }
// void println(char *buff, int buff_len) {
// 	print(buff, buff_len);
// 	printf("\n");
// }

// void query(int server_fd) {
	
// 	static int buff_len = 25;
// 	char buff[25];
// 	printf("Query: ");
// 	while(1) {
// 		bzero(buff, sizeof(buff));
// 		strncpy(buff, "prime", 5);
// 		prepare_query(buff, buff_len, 0, 50);
// 		println(buff, buff_len);

// 		write(server_fd, buff, sizeof(buff));
// 		if(strncmp(buff, "exit", 4) == 0) break;

// 		bzero(buff, sizeof(buff));
// 		read(server_fd, buff, sizeof(buff));
// 		printf("Server: ");
// 		println(buff, buff_len);
// 		printf("Query: ");
// 	}
// 	printf("Client exit\n");
// }

// // create load balancer emthod and create thread for each server fd and call query method for each server fd. and also when server stops make that thread break the query method and close server fd.
// // when server starts(monitor notifies) then create new thread(i.e call query method for thread).
// // main thread will communicate with monitor.

// void main() {
// 	struct sockaddr_in server_address;

// 	int sock_fd, flag;
// 	sock_fd = socket(AF_INET, SOCK_STREAM, 0);
// 	if(sock_fd == -1) {
// 		printf("socket creation failed");
// 		exit(0);
// 	} else printf("socket created\n");

// 	server_address.sin_family = AF_INET;
// 	// IP of server pc to connect with. INADDR_LOOPBACK is 127.0.0.1 i.e. localhost you can specify IP
// 	server_address.sin_addr.s_addr =  inet_addr("192.168.122.89"); //htonl(INADDR_ANY);
// 	// port of server process on server pc.
// 	server_address.sin_port = htons(8080);

// 	flag = connect(sock_fd, (struct sockaddr *)&server_address, sizeof(server_address));
// 	if(flag == -1) {
// 		printf("Error conecting\n");
// 		exit(0);
// 	} else printf("connected\n");

// 	query(sock_fd);
// 	close(sock_fd);
// }

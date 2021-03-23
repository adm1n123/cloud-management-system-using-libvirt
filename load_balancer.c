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
#include <time.h>
#include <signal.h>

#define SUCCESS 1
#define FAILED -1 // don't change to zero could be treated as socket_fd in connect_to_server() method.

char *STR_SUCCESS = "SUCCESS";
char *STR_FAILED = "FAILED";

// function prototypes
void *process_server_responses(void *arg); // server method to echo the client query. we can prepare server response for query.
void init_response_thread(); // creating threads and creating epoll instance for each thread.
void make_non_block_socket(int fd); // make the socket fd non blocking so that read/write on fd can be performed without blocking.
int create_lstn_sock_fd(); // create listening socket.


int auto_sclr_sock_fd; // autoscaler socket fo.
int lstn_sock_fd; // listening socket fd used to connect to auto scaler.


struct my_epoll_context { // this is custom structure used for data storation.
	int epoll_fd; // this is file descriptor of epoll instance. we will add remove socket fds using this epoll_fd.
	struct epoll_event *response_events; // when we wait on epoll then list of event will be returned(of type 'struct epoll_event') and we will store those in this memory (NOTE: we have already created memory for this pointer).
	// you can store response events anywhere but it is good to store the data related to same epoll in same structure.
} my_epoll; // only one epoll is created to receive responses from server. and any no of threads can manage it but I used one thread to monitor responses.


struct threads {
	pthread_t req_thread; // request generator threads.
	void * req_thread_args;

	pthread_t res_thread;
	void * res_thread_args;

} threads;

struct live_server_entry {
	char *IP;
	int server_sock_fd;
	bool high_load;
	struct live_server_entry* next;
} *live_serv_list = NULL;


struct request_meta {
	long request_id;
	int range_high; // request data max value.
	int range_low; // request data min value.
	int swing_delay; // could be +ve/-ve;
	unsigned int low_load_delay; // 1000 micro-second for usleep().
	unsigned int high_load_delay; // 10 micro-seconds for usleep().
	unsigned int inter_req_delay; // in micro-seconds. set to 1 seconds. let the signal handler decide.
	time_t service_start_time; // set any high value
} req_meta;

void init_req_meta() {
	req_meta.request_id = 0;
	req_meta.range_high = 1e4; // keep range smaller so that there is constant time per ops.
	req_meta.range_low = 9e3;
	req_meta.swing_delay = 0; 
	req_meta.low_load_delay = 5.5e5;
	req_meta.high_load_delay = 2e5;
	req_meta.inter_req_delay = 5.5e5; // start with low load always.
	req_meta.service_start_time = 0;
	return;
}

void print_live_servers() {
	struct live_server_entry* ptr = live_serv_list;
	printf("--------------- Printing Live Servers -----------\n");
	while(ptr != NULL) {
		printf("IP: %s, FD: %d, HIGH_LOAD: %d\n", ptr->IP, ptr->server_sock_fd, ptr->high_load);
		ptr = ptr->next;
	}
	printf("-------------------------------------------------\n");
}

struct live_server_entry* insert_server_entry(char *IP, int server_sock_fd) {
	struct live_server_entry* eptr = malloc(sizeof(struct live_server_entry));
	eptr->IP = calloc(strlen(IP)+1, sizeof(char));
	strcpy(eptr->IP, IP);
	eptr->server_sock_fd = server_sock_fd;
	eptr->high_load = false;
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
	printf("Deleting server entry IP%s:\n", IP);
	print_live_servers();
	if(live_serv_list == NULL) {
		return;
	}
	struct live_server_entry* eptr = live_serv_list;
	if(strcmp(eptr->IP, IP) == 0) {
		live_serv_list = eptr->next;
		free(eptr);
		print_live_servers();
		return;
	}
	while(eptr->next != NULL && strcmp(eptr->next->IP, IP) != 0) {
		eptr = eptr->next;
	}
	if(eptr->next == NULL) return;
	struct live_server_entry* tmp = eptr->next;
	eptr->next = eptr->next->next;
	free(tmp);
	print_live_servers();
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

static inline void update_swing() {
	req_meta.inter_req_delay += req_meta.swing_delay;

	if(req_meta.inter_req_delay < req_meta.high_load_delay) { // very high load reduce load. means increase the req delay.
		if(req_meta.swing_delay < 0) req_meta.swing_delay *= -1;
		return;
	}
	if(req_meta.inter_req_delay > req_meta.low_load_delay) { // very low load decrease delay.
		if(req_meta.swing_delay > 0) req_meta.swing_delay *= -1;
		return;
	}
	return;
}

void get_request(char *buff, int buff_len) {
	// sleep(5);
	usleep(req_meta.inter_req_delay);
	if(req_meta.swing_delay != 0) update_swing();

	long int request_data = req_meta.range_low + rand() % (req_meta.range_high - req_meta.range_low);
	sprintf(buff, "REQ_ID:%ld;REQ_DATA:%ld;", req_meta.request_id, request_data);
	req_meta.request_id += 1;

	return;
}

void *generate_requests(void *arg) {
	// TODO:
	// use system time and for given interval generate fixed number of request in round-robbin so that when new domain is spawns then no of request per domain decreases.
	static int buff_len = 100;
	char buff[buff_len];
	struct live_server_entry* ptr;
	while(true) {
		ptr = live_serv_list;
		while(ptr != NULL && ptr->high_load == false) {
			get_request(buff, buff_len);
			// printf("Writing on socket fd: %d\n", ptr->server_sock_fd);
			int flag = write(ptr->server_sock_fd, buff, buff_len);
			if(flag < 0) {
				close(ptr->server_sock_fd);
				printf("Server disconnected at IP:%s\n", ptr->IP);
				delete_server_entry(ptr->IP);
				ptr = ptr->next;
				continue;
			}
			ptr = ptr->next;
		}

		if(threads.req_thread_args != NULL) {
			break;
		}
	}
}



void init_request_thread() {
	threads.req_thread_args = NULL;
	pthread_create(&threads.req_thread, NULL, &generate_requests, NULL); // creating the thread
	return;
}

void stop_request_thread() {
	threads.req_thread_args = (void *)1;
	pthread_join(threads.req_thread, NULL);
	return;
}


void *process_server_responses(void *arg) {

	FILE *fd = fopen("response.txt", "w");
	setbuf(fd, NULL);
	time_t cur_time;
	time(&cur_time);
	fprintf(fd, "###############   Processing Server Responses Start Time: %s", ctime(&cur_time));

	static int buff_len = 100;
	char buff[buff_len];
	int nfds, len;
	while(true) {
		nfds = epoll_wait(my_epoll.epoll_fd, my_epoll.response_events, 10, 1);// 10 is the maxevents to be returned by call (we have allocated space for 10 events during epoll instance creation you can increase) 1 timeout means wait for 1 second.
		for(int i = 0; i < nfds; i++) {
			int sock_fd = my_epoll.response_events[i].data.fd;
			len = read(sock_fd, buff, sizeof(buff));
			if(len == 0) { // event occured but no data means server disconnected. don't close fd let autoscaler inform what to do.
				// printf("Server disconnected sock fd: %d\n", sock_fd);
				continue;
			}
			while(len > 0) {
				fprintf(fd, "Server response: %s\n", buff);
				// printf("Server response: %s\n", buff);
				len = read(sock_fd, buff, sizeof(buff));
			}
		}
		if(threads.res_thread_args != NULL) {
			break;
		}
	}
	fprintf(fd, "Total request sent: %ld\n", req_meta.request_id);
	time(&cur_time);
	fprintf(fd, "#####################   Processing stopped at: %s", ctime(&cur_time));
	fclose(fd);
}

void stop_response_thread() {
	threads.res_thread_args = (void *)1;
	pthread_join(threads.res_thread, NULL);
	return;
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
		printf("O_NONBLOCK failed for sock fd: %d", fd);
		exit(0);
	}
}

int create_lstn_sock_fd() {
	int flag;
	
	struct sockaddr_in lstn_socket, client_addr;
	
	int lstn_sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(lstn_sock_fd == -1) {
		printf("Listening socket creation failed\n");
		exit(0);
	} else printf("Listening socket created\n");

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
	} else printf("Listening socket is ready to accept connections.\n");
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

void destroy() {

	printf("Started destroying ...\n");
	close(lstn_sock_fd);
	printf("listening socket closed.\n");
	close(auto_sclr_sock_fd);
	printf("autoscaler socket closed\n");
	struct live_server_entry* ptr = live_serv_list;
	while(ptr != NULL) {
		close(ptr->server_sock_fd);
		printf("Server: %s socket closed\n", ptr->IP);
		ptr = ptr->next;
	}
	printf("Finished destroying.\n");
	return;
}

void signal_handler(int sig_type) {
	if(sig_type == SIGINT) {
		printf("\nEnter one choice: LOW | HIGH | SWING | EXIT\n");
		char choice[10];
		scanf("%s", choice);
		if(strcmp(choice, "LOW") == 0) {
			req_meta.inter_req_delay = req_meta.low_load_delay;
			req_meta.swing_delay = 0;
		} else 	if(strcmp(choice, "HIGH") == 0) {
			req_meta.inter_req_delay = req_meta.high_load_delay;
			req_meta.swing_delay = 0;
		} else 	if(strcmp(choice, "SWING") == 0) {
			scanf("%d", &req_meta.swing_delay);//5000 + rand() % 100;
			printf("SWING delay set to: %d micro-seconds\n", req_meta.swing_delay);
		} else 	if(strcmp(choice, "EXIT") == 0) {
			printf("Exiting\n");
			threads.req_thread_args = (void *)1;
			pthread_join(threads.req_thread, NULL);
			printf("Request thread stopped\n");
			printf("Waiting for 3 seconds for any server responses ...\n");
			sleep(3);
			threads.res_thread_args = (void *)1;
			pthread_join(threads.res_thread, NULL);
			printf("Response thread stopped\n");
			destroy();
			exit(0);
		} else {
			printf("INVALID CHOICE\n");
		}

	} else if(sig_type == SIGTERM) {
		printf("Got SIGTERM exiting\n");
		exit(0);
	} else if(sig_type == SIGPIPE) {
		printf("Got SIGPIPE\n");
	}
	return;
}

int connect_to_autoscaler() {
	int len, flag;
	struct sockaddr_in client_addr;
	len = sizeof(client_addr);
	// accept(listen_fd, NULL, NULL); we can use this also because we are not using client IP,port etc. hence no point in passing second argument. second argument is reference to structure in which connected clients IP, port is stored.
	printf("Waiting for autoscaler ...\n");
	while(true) {
		auto_sclr_sock_fd = accept(lstn_sock_fd, (struct sockaddr *)&client_addr, &len);
		if(auto_sclr_sock_fd == -1) {
			fprintf(stderr, "Error connecting autoscaler\n");
			sleep(3);
			continue;
		}
		printf("Connected to autoscaler\n");
		break;
	}
}

void scale_out(char *message, int msg_len, char *IP) {

	struct live_server_entry* ptr = get_server_entry(IP);
	
	if(ptr != NULL) { 	// already running.
		printf("Server is already running.\n");
		strcpy(message, STR_SUCCESS);
		write(auto_sclr_sock_fd, message, msg_len);
		return;
	}
	printf("Connecting ... to new server at IP:%s \n", IP);
	int server_sock_fd = connect_to_server(IP);
	if(server_sock_fd < 0) {
		strcpy(message, STR_FAILED);
		write(auto_sclr_sock_fd, message, msg_len);
		return;
	}
	make_non_block_socket(server_sock_fd); // so that response thread do not block(means entire process does not block)
	
	struct epoll_event interested_event; // struct epoll_event is inbuilt structure we just created variable of this struct type to store interested event data for this epoll instance.
	interested_event.data.fd = server_sock_fd; // adding the socket fd
	interested_event.events = EPOLLIN | EPOLLET; // adding the event type for this socket fd.
	epoll_ctl(my_epoll.epoll_fd, EPOLL_CTL_ADD, server_sock_fd, &interested_event); // adding the socket to epoll instance.
	
	strcpy(message, STR_SUCCESS);
	write(auto_sclr_sock_fd, message, msg_len);

	stop_request_thread();
	insert_server_entry(IP, server_sock_fd);
	init_request_thread();

	return;
}

void scale_in(char *message, int msg_len, char *IP) {
	struct live_server_entry* ptr = get_server_entry(IP);
	if(ptr == NULL) {
		printf("Server already disconnected at IP:%s\n", IP);
		strcpy(message, STR_SUCCESS);
		write(auto_sclr_sock_fd, message, msg_len);
		return;
	}
	// 
	if(close(ptr->server_sock_fd) == 0) { // since only of sock_fd for each IP(no multiple fds by using dup, dup2) hence closing fd will also remove from epoll context no need of epoll_ctl(EPOLL_CTL_DEL)
		strcpy(message, STR_SUCCESS);
		write(auto_sclr_sock_fd, message, msg_len);
		printf("Disconnected from server at IP:%s\n", IP);
		stop_request_thread();
		delete_server_entry(IP);
		init_request_thread();
		return;
	}
	strcpy(message, STR_FAILED);
	write(auto_sclr_sock_fd, message, msg_len);
	return;

}

void check_consistency(char *message, int msg_len, char *IP) {
	struct live_server_entry* ptr = get_server_entry(IP);

	if(ptr != NULL) { 	// already connected.
		strcpy(message, STR_SUCCESS);
		write(auto_sclr_sock_fd, message, msg_len);
		return;
	}

	// not connected scale out.
	printf("Consistency called connecting to idle server\n");
	scale_out(message, msg_len, IP);
	return;
}

void main() {

	// register signal handler in main thead so that main thread calls signal handler.
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGPIPE, signal_handler);
	
	lstn_sock_fd = create_lstn_sock_fd();
	
	connect_to_autoscaler();

	init_req_meta(); // initializing request meta data.
	
	init_request_thread(); // request generator thread.

	init_response_thread(); // response collector thread.

	static int msg_len = 50;
	char message[msg_len];
	char *TYPE;
	char *IP;

	while(true) { // talk to autoscaler.
		int flag = read(auto_sclr_sock_fd, message, msg_len);
		// printf("Reading autoscaler message:%s, flag:%d\n", message, flag);
		sleep(1);
		if(flag == 0) {
			printf("Autoscaler disconnected.\n");
			connect_to_autoscaler();
			continue;
		}
		if(flag != msg_len) {
			printf("Error reading notification from autoscaler\n");
			continue; // no message;
		}

		TYPE = strtok(message, ";");
		IP = strtok(NULL, ";");

		if(strcmp(TYPE, "SCALE_OUT") == 0) {
			scale_out(message, msg_len, IP);
			print_live_servers();
			continue;
		}
		if(strcmp(TYPE, "SCALE_IN") == 0) {
			scale_in(message, msg_len, IP);
			print_live_servers();
			continue;
		}
		if(strcmp(TYPE, "CONSISTENT") == 0) {
			check_consistency(message, msg_len, IP);
			continue;
		}
	}
	return;
}

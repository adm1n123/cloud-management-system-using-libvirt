#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <netdb.h> 
#include <netinet/in.h> 
#include <fcntl.h>
#include <string.h> 
#include <unistd.h>
#include <sys/socket.h> 
#include <sys/types.h>
#include <arpa/inet.h>
#include <libvirt/libvirt.h>


#define CPU_USAGE_HIGH 2
#define CPU_USAGE_MOD 1
#define CPU_USAGE_LOW 0

struct my_doms {	// my custom structure to store info.
	virConnectPtr conn;
	int doms_count;
	virDomainPtr *domains; // domain is pointer to structure array.
} my_doms;


struct doms_stats {
	virDomainPtr domPtr;
	struct doms_stats *next;
	unsigned long long int history;
	unsigned long long int current;
	int percent;
}


void init_server() { // make sure at least one server is started.
	int count = 0;
	for(int i = 0; i < my_doms.doms_count; i++) {
		if(virDomainIsActive(my_doms.domains[i]) == 1) {
			count += 1;
		}
	}
	if(count > 0) {
		printf("no of servers running: %d\n", count);
		return;
	}
	if(virDomainCreate(my_doms.domains[0]) == 0) {
		printf("Server started\n");
		return;
	}
	printf("Error starting server\n");
	exit(1);
}

void init() {
	my_doms.conn = virConnectOpen("qemu:///system");
	if(my_doms.conn == NULL) {
		fprintf(stderr, "Error Connecting Hypervisor\n");
		exit(1);
	}
	printf("Connected to qemu:///system\n");

	my_doms.doms_count = virConnectListAllDomains(my_doms.conn, &(my_doms.domains), 0); // flags = 0
	printf("No of domains: %d\n", my_doms.doms_count);
	if(my_doms.doms_count == 0) {
		fprintf(stderr, "Error No domains returned\n");
		exit(1);
	}

	for(int i = 0; i < my_doms.doms_count; i++) {
		printf("Domain%d name: %s\n", i, virDomainGetName(my_doms.domains[i]));
	}

	init_server();
}

void destroy() {
	virConnectClose(my_doms.conn);
	printf("Server stopped\n");
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

int connect_to_load_balancer() {
	struct sockaddr_in load_bal_address;

	int sock_fd, flag;
	sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(sock_fd == -1) {
		printf("socket creation failed");
		exit(0);
	} else printf("socket created\n");

	load_bal_address.sin_family = AF_INET;
	// IP of server pc to connect with. INADDR_LOOPBACK is 127.0.0.1 i.e. localhost you can specify IP
	load_bal_address.sin_addr.s_addr =  htonl(INADDR_ANY);
	// port of server process on server pc.
	load_bal_address.sin_port = htons(8081); // verify(server ports and load balancer ports must be different since both are in private network)

	flag = connect(sock_fd, (struct sockaddr *)&load_bal_address, sizeof(load_bal_address));
	if(flag == -1) {
		printf("Error conecting load balancer\n");
		exit(0);
	} else printf("connected to load balancer\n");

	make_non_block_socket(sock_fd);
	return sock_fd;
}

void notify_load_balancer(int server_fd) {
	
	static int buff_len = 25;
	char buff[25];
	printf("Query: ");
	while(1) {
		bzero(buff, sizeof(buff));
		strncpy(buff, "prime", 5);
		// prepare_query(buff, buff_len, 0, 50);
		// println(buff, buff_len);

		write(server_fd, buff, sizeof(buff));
		if(strncmp(buff, "exit", 4) == 0) break;

		bzero(buff, sizeof(buff));
		read(server_fd, buff, sizeof(buff));
		printf("Server: ");
		// println(buff, buff_len);
		printf("Query: ");
	}
	printf("Client exit\n");
}

void get_cpu_usage(virDomainPtr dom) {
	int nparams = virDomainGetCPUStats(dom, NULL, 0, -1, 1, 0); // nparams
	virTypedParameterPtr params = calloc(nparams, sizeof(virTypedParameter));
	virDomainGetCPUStats(dom, params, nparams, -1, 1, 0); // total stats.

	printf("Printing parameters and value\n");
	for(int i = 0; i < nparams; i++) {
		printf("%s: %lld\n", params[i].field, params[i].value.ul); // this time is in nano seconds.
	}
	return;
}

int analyse_cpu_usage() {

	get_cpu_usage(my_doms.domains[0]);
	sleep(1);


	return CPU_USAGE_MOD;
}

void scale_out() {

}

void scale_in() {

}


void main() {
	
	init();
	// int sock_fd = connect_to_load_balancer();

	while(true) {
		int load = analyse_cpu_usage();
		if(load == CPU_USAGE_HIGH) {
			scale_out(); // increase resources
		} else if(load == CPU_USAGE_LOW) {
			scale_in();
		} else {

		}
	}

	// close(sock_fd);
	destroy();
	
}
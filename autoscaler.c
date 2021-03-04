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

// CPU stats flags.
#define CPU_USAGE_HIGH 2
#define CPU_USAGE_MOD 1
#define CPU_USAGE_LOW 0

// general flags
#define SUCCESS 1
#define FAILED 0

// notification flags
#define NOTI_DOM_CRT_SUCC 100	// new domain creation successfully notified
#define NOTI_DOM_CRT_FAILD 101	// new domain creation notification failed
#define NOTI_DOM_SHTDWN_SUCC 102 // domain shutdown successfully notified
#define NOTI_DOM_SHTDWN_FAILD 103 // domain shutdown notification failed

// flags used to call notify function. TYPE params of notify_load_balancer method
#define NOTI_SCALE_OUT 1	// notify load balancer to start sending request to new domain specified in params
#define NOTI_SCALE_IN 0		// notify load balancer to stop sending request to domain specified in params


int notify_load_balancer(virDomainPtr domPtr, int TYPE);
int connect_to_load_balancer();

// Gloabal data.
int load_bal_sock_fd; // socket fd of load balancer.
virConnectPtr conn;


struct my_doms {	// my custom structure to store info.
	int doms_count;
	virDomainPtr *domains; // domain is pointer to structure array.
} my_doms;


struct doms_stats {	// list of active domains
	virDomainPtr domPtr;
	struct doms_stats *next;
	unsigned long long int history;
	unsigned long long int last;
	unsigned long long int current;
	double percent;
	int notified; // true: notified to load balancer.
} *statsPtr;


struct doms_stats* insert_dom_stat(virDomainPtr domPtr) {	// insert dom into active domains list. called when VM starts or resume
	if(domPtr == NULL) {
		printf("Invalid insert ops domPtr is NULL\n");
		return NULL;
	}
	struct doms_stats *dom_stat = malloc(sizeof(struct doms_stats));
	dom_stat->domPtr = domPtr;
	dom_stat->next = NULL;
	dom_stat->history = 0;
	dom_stat->last = 0;
	dom_stat->current = 0;
	dom_stat->percent = 0;
	dom_stat->notified = false;

	if(statsPtr == NULL) {
		statsPtr = dom_stat;
		return dom_stat;
	}
	dom_stat->next = statsPtr;
	statsPtr = dom_stat;
	return dom_stat;
}
void delete_dom_stat(virDomainPtr domPtr) {	// delete dom from active domain list called when VM is shutdown or paused.
	struct doms_stats *ptr = statsPtr;
	if(domPtr == NULL || statsPtr == NULL) {
		printf("Invalid delete ops domPtr/statsPtr is NULL\n");
		return;
	}
	if(ptr->domPtr == domPtr) {
		statsPtr = ptr->next;
		free(ptr);
		return;
	}
	while(ptr->next != NULL && ptr->next->domPtr != domPtr) {
		ptr = ptr->next;
	}
	if(ptr->next == NULL) {
		printf("Invalid delete ops domPtr not found\n");
		return;
	}
	struct doms_stats *tmp = ptr->next;
	ptr->next = ptr->next->next;
	free(tmp);
	return;
}

struct doms_stats* get_dom_stat(virDomainPtr domPtr) {
	struct doms_stats *ptr = statsPtr;
	if(domPtr == NULL) {
		printf("Invalid dom_stat get domPtr is NULL\n");
		return NULL;
	}
	while(ptr != NULL) {
		if(ptr->domPtr == domPtr) {
			return ptr;
		}
		ptr = ptr->next;
	}
	return NULL;
}

void init_server() { // make sure at least one server is started.
	/*
	to do:
	notify the load balancer about running domains IP and return success from load balancer if connected or already connected.
	*/
	int count = 0;
	for(int i = 0; i < my_doms.doms_count; i++) {
		if(virDomainIsActive(my_doms.domains[i]) == 1) {
			struct doms_stats* ptr = insert_dom_stat(my_doms.domains[i]);
			int notified = notify_load_balancer(ptr->domPtr, NOTI_SCALE_OUT);
			if(notified == SUCCESS) {
				ptr->notified = NOTI_DOM_CRT_SUCC;
			} else {
				ptr->notified = NOTI_DOM_CRT_FAILD;
			}

			printf("domain already running: %s\n", virDomainGetName(my_doms.domains[i]));
			count += 1;
		}
	}
	if(count > 0) return;

	if(virDomainCreate(my_doms.domains[0]) == 0) { // start any one domain. NOTE: domain[0] could be any domain because list domain fetches doms in no specific order.
		struct doms_stats* ptr = insert_dom_stat(my_doms.domains[0]);
		int notified = notify_load_balancer(ptr->domPtr, NOTI_SCALE_OUT);
		if(notified == SUCCESS) {
			ptr->notified = NOTI_DOM_CRT_SUCC;
			printf("Started domain: %s\n", virDomainGetName(my_doms.domains[0]));
		} else {
			ptr->notified = NOTI_DOM_CRT_FAILD;
		}
		return;
	}
	printf("Error creating domain\n");
	exit(1);
}

void init() {
	statsPtr = NULL; // active dom list

	conn = virConnectOpen("qemu:///system");
	if(conn == NULL) {
		fprintf(stderr, "Error Connecting Hypervisor\n");
		exit(1);
	}
	printf("Connected to qemu:///system\n");

	my_doms.doms_count = virConnectListAllDomains(conn, &(my_doms.domains), 0); // flags = 0
	printf("No of domains: %d\n", my_doms.doms_count);
	if(my_doms.doms_count == 0) {
		fprintf(stderr, "Error No domains returned\n");
		exit(1);
	}

	for(int i = 0; i < my_doms.doms_count; i++) {
		printf("Domain%d name: %s\n", i, virDomainGetName(my_doms.domains[i]));
	}

	
	load_bal_sock_fd = connect_to_load_balancer();
	init_server();
}

void destroy() {
	virConnectClose(conn);
	printf("Server stopped\n");
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
	load_bal_address.sin_port = htons(8181); // verify(server ports and load balancer ports must be different since both are in private network)

	flag = connect(sock_fd, (struct sockaddr *)&load_bal_address, sizeof(load_bal_address));
	if(flag == -1) {
		printf("Error conecting load balancer\n");
		exit(0);
	} else printf("connected to load balancer\n");

	return sock_fd;
}

int notify_load_balancer(virDomainPtr domPtr, int NOTI_TYPE) {
	printf("noti called\n");
	static int msg_len = 50;
	char *STR_SUCCESS = "SUCCESS";
	char *STR_FAILED = "FAILED";

	char message[msg_len]; // use strtok and send space filled message.

	char *IP = NULL; // IP of domPtr
	char *TYPE;

	printf("getting interfaces\n");
	virDomainInterfacePtr *ifaces = NULL;
	int ifaces_count = virDomainInterfaceAddresses(domPtr, &ifaces, 0, 0);
	if(ifaces_count < 0) {
		fprintf(stderr, "Error getting interfaces\n");
		return FAILED;
	}
	printf("printing addresses\n");
	virDomainIPAddressPtr ip_addr = ifaces[0]->addrs + 0; // only one interface hence ifaces[0] is used for VM IP. +0 for first entry of array of IPs of interface. 
	IP = ip_addr->addr;
	if(IP == NULL) {
		fprintf(stderr, "Error getting IP address\n");
		return FAILED;
	}
	IP = strcat(IP, ";");

	if(NOTI_TYPE == NOTI_SCALE_OUT) {
		TYPE = "SCALE_OUT;";
	}
	if(NOTI_TYPE == NOTI_SCALE_IN) {
		TYPE = "SCALE_IN;";
	}

	char *msg = strcat(TYPE, IP);
	strcpy(message, msg);

	int flag = write(load_bal_sock_fd, message, msg_len);
	if(flag != msg_len) {
		fprintf(stderr, "Error notifying\n");
		return FAILED;
	}
	flag = read(load_bal_sock_fd, message, msg_len);
	if(flag != msg_len) {
		fprintf(stderr, "Error getting ACK\n");
		return FAILED;
	}
	if(strncmp(message, STR_SUCCESS, strlen(STR_SUCCESS)) == 0) {
		return SUCCESS;
	}
	return FAILED;
}

void get_cpu_usage(struct doms_stats* ptr) {
	int nparams = virDomainGetCPUStats(ptr->domPtr, NULL, 0, -1, 1, 0); // nparams
	virTypedParameterPtr params = calloc(nparams, sizeof(virTypedParameter));
	virDomainGetCPUStats(ptr->domPtr, params, nparams, -1, 1, 0); // total stats.
	ptr->current = params[0].value.ul; // total cpu time in nano.

	// printf("Printing parameters and value\n");
	// for(int i = 0; i < nparams; i++) {
	// 	printf("%s: %lld\n", params[i].field, params[i].value.ul); // this time is in nano seconds.
	// }
	return;
}

int analyse_cpu_usage() {
	/*	
		TODO:
		dont't include those VMs which are created but not notifed in calculation.
	*/
	double avg_load = 0;
	int dom_count = 0;
	struct doms_stats *ptr = statsPtr;
	while(ptr != NULL) {
		ptr->history = ptr->current - ptr->last;
		get_cpu_usage(ptr);
		ptr->last = ptr->current;
		ptr = ptr->next;
	}
	sleep(1); // 1000 msec. if sleeping for n seconds divide time difference by n during calulation.
	ptr = statsPtr;
	printf("got it1\n");
	while(ptr != NULL) {
		get_cpu_usage(ptr);
		double cur_per = 0.40 * (ptr->history / 1.0e9) + 0.60 * ((ptr->current - ptr->last) / 1.0e9); // divide by nano sec to get time spend per second.
		cur_per /= 1.2;	// our thread sleeps for 1.2 sec (that difference between two reading but thread nearly sleeps for 1 sec.)
		// time difference is actually sum of time difference of all the CPUs allocated so if you allocated more than one CPUs then dynamically check how many CPUs allocated
		// to domain currently and then divide by that. NOTE: if one cpu is allocated cur_per == 1.0(approx) if 2 CPUs allocated to domain cur_per = 2.0(approx).
		// if both VMs runs together then one CPU is allocated to each because there are not enough CPUs(PC has total 4 hence 3 cannot be allocated to VMs) so cur_per for both VMs is 1.0(approx).

		ptr->percent = 0.30 * ptr->percent + 0.70 * cur_per;
		avg_load += ptr->percent;
		dom_count += 1;

		printf("Domain: %s, current usage: %lf\n", virDomainGetName(ptr->domPtr), cur_per);

		ptr = ptr->next;
	}

	

	avg_load /= dom_count;
	printf("no of doms: %d, 	avg_load %lf\n", dom_count, avg_load);

	if(avg_load > 0.60) return CPU_USAGE_HIGH;
	if(avg_load > 0.40) return CPU_USAGE_MOD;
	return CPU_USAGE_LOW;
}

void scale_out() {
	// check already created but not notified doms
	struct doms_stats* sptr = statsPtr;
	while(sptr != NULL) {
		if(sptr->notified == NOTI_DOM_CRT_FAILD) {
			int notified = notify_load_balancer(sptr->domPtr, NOTI_SCALE_OUT); // start sending request to this.
			if(notified == SUCCESS) {
				sptr->notified = NOTI_DOM_CRT_SUCC;
			}
			return;
		}
		sptr = sptr->next;
	}

	// create new domain
	virDomainPtr domPtr = NULL;
	for(int i = 0; i < my_doms.doms_count; i++) {
		if(virDomainIsActive(my_doms.domains[i]) == 0 && 	// 0: inactive  1: active  -1: error.
			virDomainCreate(my_doms.domains[i]) == 0) { 	// 0: success.
				domPtr = my_doms.domains[i];
				break;
		} 
	}
	if(domPtr == NULL) {
		printf("Not enough domains to scale out\n");
		return;
	}
	struct doms_stats* ptr = insert_dom_stat(domPtr);
	int notified = notify_load_balancer(sptr->domPtr, NOTI_SCALE_OUT); // start sending request to this.
	if(notified == SUCCESS) {
		ptr->notified = NOTI_DOM_CRT_SUCC;
		printf("Domain created: %s\n", virDomainGetName(domPtr));
	} else {
		ptr->notified = NOTI_DOM_CRT_FAILD;
	}
		
	return;
}


void scale_in() {
	// check already stopped but not notified doms
	struct doms_stats* sptr = statsPtr;
	while(sptr != NULL) {
		if(sptr->notified == NOTI_DOM_SHTDWN_FAILD) {
			int notified = notify_load_balancer(sptr->domPtr, NOTI_SCALE_IN); // stop sending request to this.
			if(notified == SUCCESS) {
				sptr->notified = NOTI_DOM_SHTDWN_SUCC;
				delete_dom_stat(sptr->domPtr);
			}
			return;
		}
		sptr = sptr->next;
	}
	if(virConnectNumOfDomains(conn) <= 1) { // number of active domains. don't stop all the domains.
		return;
	}

	virDomainPtr domPtr = NULL;
	for(int i = 0; i < my_doms.doms_count; i++) {
		if(virDomainIsActive(my_doms.domains[i]) == 1) { 	// 1: active.
				domPtr = my_doms.domains[i]; 			// get any one domain to shutdown.
				break;
		} 
	}

	int notified = notify_load_balancer(domPtr, NOTI_SCALE_IN); // stop sending request domPtr;
		/*
	to do:
	notify the load balancer about stopping domains IP and return success from load balancer if TCP connection stopped or already stopped.
	*/

	if(notified == SUCCESS && virDomainShutdown(domPtr) == 0) { // 0: success
		delete_dom_stat(domPtr);
		printf("Domain shutdown: %s\n", virDomainGetName(domPtr));
	}
}


void main() {
	
	init();

	while(true) {
		int load = analyse_cpu_usage();

		if(load == CPU_USAGE_HIGH) {
			printf("CPU Usage High\n");
			scale_out(); // increase resources
		} else if(load == CPU_USAGE_LOW) {
			printf("CPU Usage Low\n");
			scale_in();
		} else {
			printf("CPU Usage Moderate\n");
		}
		sleep(5);
	}

	// close(sock_fd);
	destroy();
	
}
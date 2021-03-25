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
#include <pthread.h>

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
#define NOTI_CONSISTENT 2	// notify load balancer that sever is running it must be serving request.

int notify_load_balancer(virDomainPtr domPtr, int TYPE);
int connect_to_load_balancer();
void scale_out();
void scale_in();
void stablize_cpu_usage();

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
	unsigned long long int llast; // total cpu time when measured 2nd last time
	unsigned long long int last;	// total cpu time when measured last time
	unsigned long long int current;	// total cpu time when measured this time.
	double cpu_percent;	// calculated using cpu usage see server process in top.
	int notified; // 4 notification flags default false.
} *statsPtr;


struct doms_stats* insert_dom_stat(virDomainPtr domPtr) {	// insert dom into active domains list. called when VM starts or resume
	if(domPtr == NULL) {
		printf("Invalid insert ops domPtr is NULL\n");
		return NULL;
	}
	struct doms_stats *dom_stat = malloc(sizeof(struct doms_stats));
	dom_stat->domPtr = domPtr;
	dom_stat->next = NULL;
	dom_stat->llast = 0;
	dom_stat->last = 0;
	dom_stat->current = 0;
	dom_stat->cpu_percent = 0;
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

	int count = 0;
	for(int i = 0; i < my_doms.doms_count; i++) {
		if(virDomainIsActive(my_doms.domains[i]) == 1) {	// inform already running domains
			printf("Domain already running: %s\n", virDomainGetName(my_doms.domains[i]));
			struct doms_stats* ptr = insert_dom_stat(my_doms.domains[i]);
			int notified = notify_load_balancer(ptr->domPtr, NOTI_SCALE_OUT);
			if(notified == SUCCESS) {
				ptr->notified = NOTI_DOM_CRT_SUCC;
			} else {
				ptr->notified = NOTI_DOM_CRT_FAILD;
			}
			count += 1;
		}
	}
	if(count > 0) return;
	
	if(virDomainCreate(my_doms.domains[0]) == 0) { 	// 0: success.
		virDomainPtr domPtr = my_doms.domains[0];
		while(notify_load_balancer(domPtr, NOTI_SCALE_OUT) != SUCCESS) sleep(3); // start sending request to this.

		struct doms_stats *sptr = insert_dom_stat(domPtr);
		sptr->notified = NOTI_DOM_CRT_SUCC;
		// just wait for sometime let domain serve some requests.
		printf("Waiting for boot up cpu usage to stabilize 10 seconds...\n"); // cpu usage is high during boot so it might trigger scale out again.
		sleep(10); // delay calculating cpu usage because it is high just wait.
		return;
	}
	printf("Domain creation failed\n");
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
		fprintf(stderr, "Error no domains found\n");
		exit(1);
	}
	if(my_doms.doms_count > 2) {
		printf("Considering only two domains out of %d\n", my_doms.doms_count);
		my_doms.doms_count = 2;
	}
	for(int i = 0; i < my_doms.doms_count; i++) {
		printf("Domain%d name: %s\n", i, virDomainGetName(my_doms.domains[i]));
	}
	
	load_bal_sock_fd = connect_to_load_balancer();
	init_server();
	return;
}

void destroy() {
	virConnectClose(conn);
	printf("Server stopped\n");
	return;
}

int connect_to_load_balancer() {
	struct sockaddr_in load_bal_address;

	int sock_fd, flag;
	sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(sock_fd == -1) {
		printf("Socket creation failed");
		exit(0);
	}

	load_bal_address.sin_family = AF_INET;
	// IP of server pc to connect with. INADDR_LOOPBACK is 127.0.0.1 i.e. localhost you can specify IP
	load_bal_address.sin_addr.s_addr =  htonl(INADDR_ANY);
	// port of server process on server pc.
	load_bal_address.sin_port = htons(8181); // verify(server ports and load balancer ports must be different since both are in private network)

	flag = connect(sock_fd, (struct sockaddr *)&load_bal_address, sizeof(load_bal_address));
	if(flag == -1) {
		printf("Error conecting load balancer\n");
		exit(0);
	} else printf("Connected to load balancer\n");

	return sock_fd;
}

int notify_load_balancer(virDomainPtr domPtr, int NOTI_TYPE) {
	// printf("noti called\n");
	static int msg_len = 50;
	char *STR_SUCCESS = "SUCCESS";
	char *STR_FAILED = "FAILED";

	char message[msg_len]; // use strtok and send space filled message.

	char *IP = NULL; // IP of domPtr
	char *TYPE;

	// printf("getting interfaces\n");
	virDomainInterfacePtr *ifaces = NULL;
	int ifaces_count;
	do {
		sleep(1);	// wait for a second don't busy wait.
		ifaces_count = virDomainInterfaceAddresses(domPtr, &ifaces, 0, 0);
	}while(ifaces == NULL);		// when machine is booting it gives NULL sometimes.
	
	if(ifaces_count < 0) {
		printf("Error getting interfaces\n");
		return FAILED;
	}

	virDomainIPAddressPtr ip_addr = ifaces[0]->addrs + 0; // only one interface hence ifaces[0] is used for VM IP. +0 for first entry of array of IPs of interface. 
	
	IP = ip_addr->addr;
	if(IP == NULL) {
		fprintf(stderr, "Error getting IP address\n");
		return FAILED;
	}

	IP = strcat(IP, ";");
	if(NOTI_TYPE == NOTI_SCALE_OUT) {
		TYPE = "SCALE_OUT;";
	} else if(NOTI_TYPE == NOTI_SCALE_IN) {
		TYPE = "SCALE_IN;";
	} else if(NOTI_TYPE == NOTI_CONSISTENT) {
		TYPE = "CONSISTENT;";
	}
	printf("Notifying server IP:%s to load_balancer for NOTI_TYPE: %s\n", IP, TYPE);

	strcpy(message, TYPE);
	strcat(message, IP);	

	int flag = write(load_bal_sock_fd, message, msg_len);
	if(flag != msg_len) {
		fprintf(stderr, "Error notifying\n");
		return FAILED;
	}

	flag = read(load_bal_sock_fd, message, msg_len);
	// printf("NOTI response from load balancer: %s, flag:%d, message length:%ld\n", message, flag, sizeof(message));
	if(flag != msg_len) {
		fprintf(stderr, "Error getting ACK\n");
		return FAILED;
	}
	
	if(strncmp(message, STR_SUCCESS, strlen(STR_SUCCESS)) == 0) {
		printf("NOTI SUCCESS\n");
		return SUCCESS;
	}
	printf("NOTI FAILED\n");
	return FAILED;
}

unsigned long long int get_total_cpu_time(struct doms_stats* ptr) {
	int nparams = virDomainGetCPUStats(ptr->domPtr, NULL, 0, -1, 1, 0); // nparams
	virTypedParameterPtr params = calloc(nparams, sizeof(virTypedParameter));
	virDomainGetCPUStats(ptr->domPtr, params, nparams, -1, 1, 0); // total stats.
	
	// printf("Printing parameters and value\n");
	// for(int i = 0; i < nparams; i++) {
	// 	printf("%s: %lld\n", params[i].field, params[i].value.ul); // this time is in nano seconds.
	// }
	return params[0].value.ul; // total cpu time in nano seconds since boot(system setup may be it depends).
}

unsigned long long int get_guest_cpu_time(struct doms_stats* ptr) {
	int nparams = virDomainGetCPUStats(ptr->domPtr, NULL, 0, -1, 1, 0); // nparams
	virTypedParameterPtr params = calloc(nparams, sizeof(virTypedParameter));
	virDomainGetCPUStats(ptr->domPtr, params, nparams, -1, 1, 0); // total stats.
	unsigned long long int guest_time = params[0].value.ul - (params[1].value.ul + params[2].value.ul);	// guest time = total - (user + system)
	return guest_time > 0? guest_time: 0;	// somtimes guest time is -ve so to avoid overflow.
}

unsigned long long int get_cpu_time_interval(struct doms_stats* ptr, int seconds) {
	unsigned long long int begin = get_guest_cpu_time(ptr);
	sleep(seconds);
	unsigned long long int end = get_guest_cpu_time(ptr);
	return (end - begin) > 0? end-begin: 0;
}

int analyse_cpu_usage() {

	double avg_cpu_per = 0;
	int dom_count = 0;
	struct doms_stats *ptr = statsPtr;
	while(ptr != NULL) {
		/*	
		dont't include those VMs which are created but not notifed in calculation.
		*/	
		if(ptr->notified == NOTI_DOM_CRT_FAILD) {
			ptr = ptr->next;
			continue; // since it is booting so don't include.
		}
		ptr->llast = ptr->last;
		ptr->last = ptr->current;
		ptr->current = get_cpu_time_interval(ptr, 1); // 1000 msec. if sleeping for n seconds divide time difference by n during calulation.

		double avg_cpu_time = 0.20*(ptr->llast / 1.0e9) + 0.40*(ptr->last / 1.0e9) + 0.40*(ptr->current / 1.0e9); // divide by nano sec to get time spend per second.
		double cur_cpu_per = avg_cpu_time / 1.00;	// our thread sleeps for 1.2 sec (that difference between two reading but thread nearly sleeps for 1 sec.)
		// time difference is actually sum of time difference of all the CPUs allocated so if you allocated more than one CPUs then dynamically check how many CPUs allocated
		// to domain currently and then divide by that. NOTE: if one cpu is allocated cur_per == 1.0(approx) if 2 CPUs allocated to domain cur_per = 2.0(approx).
		// if both VMs runs together then one CPU is allocated to each because there are not enough CPUs(PC has total 4 hence 3 cannot be allocated to VMs) so cur_per for both VMs is 1.0(approx).

		ptr->cpu_percent = 0.00 * ptr->cpu_percent + 1.00 * cur_cpu_per; // considering long history with small factor.
		avg_cpu_per += ptr->cpu_percent;
		dom_count += 1;

		printf("Domain: %s, %%cpu : %lf\n", virDomainGetName(ptr->domPtr), ptr->cpu_percent * 100);
		ptr = ptr->next;
	}

	avg_cpu_per /= dom_count;
	printf("Number of doms: %d, 	avg %%cpu %lf\n", dom_count, avg_cpu_per * 100);

	if(avg_cpu_per > 0.80) return CPU_USAGE_HIGH;
	if(avg_cpu_per > 0.40) return CPU_USAGE_MOD;
	return CPU_USAGE_LOW;
}

void stablize_cpu_usage(int rounds) {
	for(int i = 0; i < rounds; i++) {
		analyse_cpu_usage();
	}
}

bool is_noti_dom_crt_faild() {	// check already created but not notified doms
	struct doms_stats* sptr = statsPtr;
	while(sptr != NULL) {
		if(sptr->notified == NOTI_DOM_CRT_FAILD) {
			int notified = notify_load_balancer(sptr->domPtr, NOTI_SCALE_OUT); // start sending request to this.
			if(notified == SUCCESS) {
				sptr->notified = NOTI_DOM_CRT_SUCC;
				// just wait for sometime let domain serve some requests.
				stablize_cpu_usage(5);
			}
			return true;
		}
		sptr = sptr->next;
	}
	return false;
}

void scale_out() {
	
	if(is_noti_dom_crt_faild() == true) return; // make sure all the created doms are notified before creating any new dom.

	// create new domain
	printf("Finding new domain to start.\n");
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
	printf("Got new domain to scale out\n");
	struct doms_stats* sptr = insert_dom_stat(domPtr);
	// sleep(20); // wait notify give segmentation fault while reading interfaces when machine is booting.
	int notified = notify_load_balancer(sptr->domPtr, NOTI_SCALE_OUT); // start sending request to this.
	if(notified == SUCCESS) {
		sptr->notified = NOTI_DOM_CRT_SUCC;
		stablize_cpu_usage(5);
		printf("Domain created: %s\n", virDomainGetName(domPtr));
	} else {
		sptr->notified = NOTI_DOM_CRT_FAILD;
	}
	return;
}


void scale_in() {
	// check already stopped but not notified doms

	if(is_noti_dom_crt_faild() == true) return; // all domain created must be notified before shuting down any random domain. it might be possible avg usage is low and connected one is shutdown.

	struct doms_stats* sptr = statsPtr;
	while(sptr != NULL) {
		if(sptr->notified == NOTI_DOM_SHTDWN_FAILD) {
			int notified = notify_load_balancer(sptr->domPtr, NOTI_SCALE_IN); // stop sending request to this.

			if(notified == SUCCESS && 
				virDomainShutdown(sptr->domPtr) == 0) { // 0: success
					delete_dom_stat(sptr->domPtr);
					printf("Shutting down domain: %s\n", virDomainGetName(sptr->domPtr));
					stablize_cpu_usage(3);
			}
			return; // don't shutdown if any one noti is pending.
		}
		sptr = sptr->next;
	}
	if(virConnectNumOfDomains(conn) <= 1) { // number of active domains. don't stop all the domains.
		return;
	}

	virDomainPtr domPtr = NULL;
	for(int i = 0; i < my_doms.doms_count; i++) {
		if(virDomainIsActive(my_doms.domains[i]) == 1) { 	// virDomainState see the state it should not be shuting down state. but I am using doms_stats list to verify this.
				domPtr = my_doms.domains[i]; 			// get any one domain to shutdown.
				struct doms_stats* tmp = get_dom_stat(domPtr);
				if(tmp == NULL) return; // wait until machine properly shutdown because entry is only deleted when machine is being shutdown.
				break;
		} 
	}
	int notified = notify_load_balancer(domPtr, NOTI_SCALE_IN); // inform to stop sending request
	sptr = get_dom_stat(domPtr);
	sptr->notified = NOTI_DOM_SHTDWN_FAILD; // if noti success and domain shutdown success then only remove entry from live servers
	if(notified == SUCCESS && 
		virDomainShutdown(domPtr) == 0) { // 0: success
			delete_dom_stat(domPtr);
			printf("Shutting down domain: %s\n", virDomainGetName(domPtr));
			stablize_cpu_usage(3);
	}
	return;
}


void *maintain_consistency(void *args) {
	
	while(true) {
		sleep(10);
		for(int i = 0; i < my_doms.doms_count; i++) {
			if(virDomainIsActive(my_doms.domains[i]) == 1) { // nofify that is it connected or not.
				virDomainPtr domPtr = my_doms.domains[i];
				int notified = notify_load_balancer(domPtr, NOTI_CONSISTENT);
				if(notified == SUCCESS) {
					struct doms_stats *sptr = get_dom_stat(domPtr);
					if(sptr == NULL) { // live but not in the live list add it. should never occur though.
						sptr = insert_dom_stat(domPtr);
						printf("Inconsistency resolved domain: %s added to live list\n", virDomainGetName(sptr->domPtr));
					} else if(sptr->notified == NOTI_DOM_CRT_FAILD) {
						printf("Inconsistency resolved idle domain: %s notified to load balancer\n", virDomainGetName(sptr->domPtr));
					}
					sptr->notified = NOTI_DOM_CRT_SUCC;
				} else {
					// domain might be booting up. don't do anything.
				}
			}
		}
	}
}


void main() {
	
	init();

	pthread_t const_thread;
	pthread_create(&const_thread, NULL, &maintain_consistency, NULL);

	int HIGH_PATIENCE = 3;
	int LOW_PATIENCE = 3;

	int high_count = 0;
	int low_count = 0;
	while(true) {
		int load = analyse_cpu_usage();
		if(load == CPU_USAGE_HIGH) {
			printf("CPU Usage High\n");
			low_count = 0;
			high_count += 1;
			if(high_count > HIGH_PATIENCE)
				scale_out(); // increase resources

		} else if(load == CPU_USAGE_LOW) {
			printf("CPU Usage Low\n");
			high_count = 0;
			low_count += 1;
			if(low_count > LOW_PATIENCE)
				scale_in();
	
		} else {
			printf("CPU Usage Moderate\n");
			low_count = 0;
			high_count = 0;
		}
		sleep(5);
	}

	// close(sock_fd);
	destroy();
	
}
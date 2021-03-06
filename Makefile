

load_balancer: load_balancer.c
	gcc -o load_balancer load_balancer.c -lpthread

autoscaler: autoscaler.c
	gcc -o autoscaler autoscaler.c -lvirt -lpthread

server: server.c server.h
	gcc -o server server.c -lpthread


bool is_prime(long n);
void sum_prime(char *buff, int n);

void print(char *buff, int len) {
	for(int i = 0; i < len; i++) printf("%c", buff[i]);
}
void println(char *buff, int buff_len) {
	print(buff, buff_len);
	printf("\n");
}


bool is_prime(long n) {
	if(n < 2) return false;
	for(int i = 2; i < n; i++) {
		if(n % i == 0) return false;
	}
	return true;
}

void sum_prime(char *buff, int len) {
	char tmp[100];
	strcpy(tmp, buff);
	
	strtok(tmp, ";");
	strtok(NULL, ":");
	char *t = strtok(NULL, ";");

	long num = strtol(t, NULL, 10); // base 10.

	// printf("\nFound num query:%ld, ", num);
	long sum = 0;
	for(int i = 2; i <= num; i++) {
		if(is_prime(i) == true) {
			sum += i;
			// printf("prime: %d, ", i);
		}
	}
	sprintf(tmp, "%sRES_DATA:%ld;", buff, sum);
	strcpy(buff, tmp);
	return;
}

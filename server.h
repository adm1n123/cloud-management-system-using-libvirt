

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
	if(strncmp(buff, "prime", 5) != 0) {
		printf("Error processing request\n");
		strncpy(buff, "Error", 5);
		return;
	}

	long num = 0;
	for(int i = 5; i < len; i++) {
		if(buff[i] != '\0') {
			num *= 10;
			num += buff[i] - '0';
		}
	}

	// printf("\nFound num query:%ld, ", num);
	long sum = 0;
	for(int i = 2; i <= num; i++) {
		if(is_prime(i) == true) {
			sum += i;
			// printf("prime: %d, ", i);
		}
	}
	// printf("\n");
	int i = len-1;
	while(sum > 0) {
		buff[i--] = sum % 10 + '0';
		sum /= 10;
	}
	while(i >= 0) buff[i--] = '\0';
	strncpy(buff, "sum", 3);
}

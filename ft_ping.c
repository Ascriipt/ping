#include <netinet/ip_icmp.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

int gen_icmpsock( ) {
	int sockfd;

	sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (sockfd < 0) {
		perror("Error: Socket creation failed.");
		exit(1);
	}

	return  sockfd;
};

int main(int ac, char **av) {

	(void)ac;
	(void)av;

	int sockfd = gen_icmpsock( );

	printf("Sock is: %d", sockfd);
	return  0;
}
#include <netinet/ip_icmp.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <math.h>

#define PING_DATALEN 56
#define ICMP_HDRLEN  8
#define PING_PKTLEN  (ICMP_HDRLEN + PING_DATALEN)

#define DBG(fmt, ...) if (G_DEBUG) fprintf(stderr, "\033[31m[debug]\033[0m %s: " fmt "\n", __func__, ##__VA_ARGS__)

typedef struct s_icmp_header {
	uint8_t  type;
	uint8_t  code;
	uint16_t checksum;
	uint16_t identifier;
	uint16_t sequence_number;
} t_icmp_header;

typedef struct s_flags {
	bool debug;
	bool bell;
	bool limit;
} t_flags;

typedef struct s_ping {
	int                 sockfd;
	struct sockaddr_in  dest;
	char                dest_ip[INET_ADDRSTRLEN];
	const char         *hostname;
	uint16_t            id;
	uint16_t            seq;
	unsigned int        sent;
	unsigned int        received;
	double              rtt_min;
	double              rtt_max;
	double              rtt_sum;
	double              rtt_sumsq;
} t_ping;

static int G_DEBUG = 0;
static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig) {
	(void)sig;
	g_running = 0;
}


int gen_icmpsock(void) {
	int sockfd;

	DBG("creating raw ICMP socket");
	sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (sockfd < 0) {
		perror("socket");
		exit(1);
	}
	return sockfd;
}

static uint16_t checksum(void *data, size_t len) {
	uint16_t *words = data;
	uint32_t  sum = 0;

	DBG("len=%zu data=%p", len, data);

	while (len > 1) {
		sum += *words++;
		len -= 2;
	}
	if (len == 1)
		sum += *(uint8_t *)words;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

static double timeval_diff_ms(struct timeval *start, struct timeval *end) {
	DBG("start=%ld.%06d end=%ld.%06d",
		(long)start->tv_sec, (int)start->tv_usec,
		(long)end->tv_sec, (int)end->tv_usec);
	return (double)(end->tv_sec - start->tv_sec) * 1000.0
		+ (double)(end->tv_usec - start->tv_usec) / 1000.0;
}

static int resolve_host(const char *host, t_ping *ping) {
	struct addrinfo hints;
	struct addrinfo *res;
	int             err;

	DBG("host=%s", host);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_RAW;
	hints.ai_protocol = IPPROTO_ICMP;
	err = getaddrinfo(host, NULL, &hints, &res);
	if (err != 0) {
		fprintf(stderr, "ft_ping: unknown host %s\n", host);
		return -1;
	}
	memcpy(&ping->dest, res->ai_addr, sizeof(ping->dest));
	inet_ntop(AF_INET, &ping->dest.sin_addr, ping->dest_ip, sizeof(ping->dest_ip));
	freeaddrinfo(res);
	return 0;
}

static int send_echo(t_ping *ping) {
	unsigned char   packet[PING_PKTLEN];
	t_icmp_header  *icmp;
	struct timeval  now;
	ssize_t         n;

	DBG("seq=%u dest=%s", ping->seq, ping->dest_ip);
	memset(packet, 0, sizeof(packet));
	icmp = (t_icmp_header *)packet;
	icmp->type = ICMP_ECHO;
	icmp->code = 0;
	icmp->checksum = 0;
	icmp->identifier = htons(ping->id);
	icmp->sequence_number = htons(ping->seq);

	gettimeofday(&now, NULL);
	memcpy(packet + ICMP_HDRLEN, &now, sizeof(now));
	for (size_t i = sizeof(now); i < PING_DATALEN; i++)
		packet[ICMP_HDRLEN + i] = (unsigned char)i;

	icmp->checksum = checksum(packet, sizeof(packet));
	DBG("sending packet to %s", ping->dest_ip);
	n = sendto(ping->sockfd, packet, sizeof(packet), 0,
		(struct sockaddr *)&ping->dest, sizeof(ping->dest));
	if (n < 0) {
		perror("sendto");
		return -1;
	}
	ping->sent++;
	ping->seq++;
	return 0;
}

static void record_rtt(t_ping *ping, double rtt) {
	DBG("rtt=%.3f ms received=%u", rtt, ping->received);
	if (ping->received == 1) {
		ping->rtt_min = rtt;
		ping->rtt_max = rtt;
	} else {
		if (rtt < ping->rtt_min)
			ping->rtt_min = rtt;
		if (rtt > ping->rtt_max)
			ping->rtt_max = rtt;
	}
	ping->rtt_sum += rtt;
	ping->rtt_sumsq += rtt * rtt;
}

static int recv_reply(t_ping *ping) {
	unsigned char      buf[1024];
	struct sockaddr_in from;
	socklen_t          fromlen = sizeof(from);
	ssize_t            n;
	struct ip         *ip;
	t_icmp_header     *icmp;
	unsigned int       iphdrlen;
	struct timeval     now;
	struct timeval     sent_at;
	double             rtt;

	DBG("waiting on sockfd=%d", ping->sockfd);
	n = recvfrom(ping->sockfd, buf, sizeof(buf), 0,
		(struct sockaddr *)&from, &fromlen);
	if (n < 0) {
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		perror("recvfrom");
		return -1;
	}
	if ((size_t)n < sizeof(struct ip) + ICMP_HDRLEN)
		return 0;

	ip = (struct ip *)buf;
	iphdrlen = ip->ip_hl * 4;
	if ((size_t)n < iphdrlen + ICMP_HDRLEN)
		return 0;

	icmp = (t_icmp_header *)(buf + iphdrlen);
	if (icmp->type != ICMP_ECHOREPLY)
		return 0;
	if (ntohs(icmp->identifier) != ping->id)
		return 0;

	gettimeofday(&now, NULL);
	rtt = 0.0;
	if ((size_t)n >= iphdrlen + ICMP_HDRLEN + sizeof(struct timeval)) {
		memcpy(&sent_at, buf + iphdrlen + ICMP_HDRLEN, sizeof(sent_at));
		rtt = timeval_diff_ms(&sent_at, &now);
	}
	ping->received++;
	record_rtt(ping, rtt);

	printf("%ld bytes from %s: icmp_seq=%u ttl=%u time=%.3f ms\n",
		(long)(n - iphdrlen),
		inet_ntoa(from.sin_addr),
		ntohs(icmp->sequence_number),
		ip->ip_ttl,
		rtt);
	fflush(stdout);
	return 1;
}

static void print_stats(t_ping *ping) {
	double loss;
	double avg;
	double stddev;

	DBG("sent=%u received=%u", ping->sent, ping->received);
	printf("\n--- %s ping statistics ---\n", ping->hostname);
	loss = 0.0;
	if (ping->sent > 0)
		loss = 100.0 * (ping->sent - ping->received) / ping->sent;
	printf("%u packets transmitted, %u packets received, %.1f%% packet loss\n",
		ping->sent, ping->received, loss);
	if (ping->received > 0) {
		avg = ping->rtt_sum / ping->received;
		stddev = 0.0;
		if (ping->received > 1) {
			stddev = ping->rtt_sumsq / ping->received - avg * avg;
			if (stddev < 0.0)
				stddev = 0.0;
			stddev = sqrt(stddev);
		}
		printf("round-trip min/avg/max/stddev = %.3f/%.3f/%.3f/%.3f ms\n",
			ping->rtt_min, avg, ping->rtt_max, stddev);
	}
}

static void ping_loop(t_ping *ping) {
	struct timeval last_send;
	struct timeval now;
	struct timeval timeout;
	fd_set         readfds;
	int            ready;
	double         elapsed;

	DBG("start dest=%s", ping->dest_ip);
	memset(&last_send, 0, sizeof(last_send));
	while (g_running) {
		gettimeofday(&now, NULL);
		elapsed = timeval_diff_ms(&last_send, &now);
		if (last_send.tv_sec == 0 || elapsed >= 1000.0) {
			if (send_echo(ping) < 0)
				break;
			last_send = now;
			elapsed = 0.0;
		}
		timeout.tv_sec = 0;
		timeout.tv_usec = (suseconds_t)((1000.0 - elapsed) * 1000.0);
		if (timeout.tv_usec < 0)
			timeout.tv_usec = 0;
		if (timeout.tv_usec >= 1000000)
			timeout.tv_usec = 999999;
		FD_ZERO(&readfds);
		FD_SET(ping->sockfd, &readfds);
		ready = select(ping->sockfd + 1, &readfds, NULL, NULL, &timeout);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			perror("select");
			break;
		}
		if (ready > 0)
			recv_reply(ping);
	}
	print_stats(ping);
}

int main(int ac, char **av) {
	t_ping ping;
	int i = -1;

	DBG("ac=%d", ac);
	if (ac != 2) {
		fprintf(stderr, "Oops buddy, that's not how you use me, for further information, type './ft_ping --help'\n");
		return 1;
	}

	while (av[++i]) {
		printf("av=%s\n", av[i]);
		if (strcmp(av[i], "--debug") == 0) {
			G_DEBUG = 1;
		}
	}

	if (strcmp(av[1], "--help") == 0) {
		printf("Usage: ft_ping [options] <hostname>\n");
		printf("Options:\n");
		printf("  -h, --help     Show this help message and exit\n");
		printf("  -v, --version  Show version information and exit\n");
		return 0;
	}

	memset(&ping, 0, sizeof(ping));
	ping.hostname = av[1];
	printf("hostname: %s\n", ping.hostname);
	ping.id = (uint16_t)getpid();
	ping.seq = 0;
	ping.rtt_min = 0.0;

	if (resolve_host(av[1], &ping) < 0)
		return 1;

	ping.sockfd = gen_icmpsock();

	signal(SIGINT, on_sigint);

	printf("PING %s (%s): %d data bytes\n",
		ping.hostname, ping.dest_ip, PING_DATALEN);
	fflush(stdout);

	ping_loop(&ping);
	close(ping.sockfd);
	return 0;
}

#define _DEFAULT_SOURCE
#include <netinet/ip_icmp.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

#include "class.h"
#include "parse.h"

static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig)
{
	(void)sig;
	g_running = 0;
}

static double elapsed_ms(const struct timeval *a, const struct timeval *b) {
	return (b->tv_sec - a->tv_sec) * 1000.0
		+ (b->tv_usec - a->tv_usec) / 1000.0;
}

static double elapsed_sec(const struct timeval *a, const struct timeval *b) {
	return elapsed_ms(a, b) / 1000.0;
}

static uint16_t icmp_checksum(const unsigned char *data, size_t len) {
	uint32_t sum = 0;
	while (len > 1) {
		sum += (uint16_t)((data[0] << 8) | data[1]);
		data += 2;
		len -= 2;
	}
	if (len)
		sum += (uint16_t)(data[0] << 8);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

static int resolve_host(t_ping *p)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	int error;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	error = getaddrinfo(p->opt.host, NULL, &hints, &res);
	if (error || !res) {
		fprintf(stderr, "ft_ping: cannot resolve '%s': %s\n", p->opt.host,
			error ? gai_strerror(error) : "unknown host");
		return -1;
	}
	memcpy(&p->dest, res->ai_addr, sizeof(p->dest));
	freeaddrinfo(res);
	if (!inet_ntop(AF_INET, &p->dest.sin_addr, p->address, sizeof(p->address)))
		return -1;
	if (p->opt.numeric
		|| getnameinfo((struct sockaddr *)&p->dest, sizeof(p->dest), p->name,
			sizeof(p->name), NULL, 0, NI_NAMEREQD) != 0) {
		strncpy(p->name, p->address, sizeof(p->name) - 1);
		p->name[sizeof(p->name) - 1] = '\0';
	}
	return 0;
}

static int open_socket(t_ping *p)
{
	int one = 1;
	unsigned char timestamp[40];

	p->fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (p->fd < 0) {
		fprintf(stderr, "ft_ping: cannot open ICMP socket: %s\n", strerror(errno));
		return -1;
	}
	if (p->opt.bypass
		&& setsockopt(p->fd, SOL_SOCKET, SO_DONTROUTE, &one, sizeof(one)) < 0)
		fprintf(stderr, "ft_ping: warning: cannot bypass routing: %s\n",
			strerror(errno));
	if (p->opt.have_ttl
		&& setsockopt(p->fd, IPPROTO_IP, IP_TTL, &p->opt.ttl, sizeof(p->opt.ttl)) < 0)
		fprintf(stderr, "ft_ping: warning: cannot set TTL: %s\n", strerror(errno));
	if (p->opt.have_tos
		&& setsockopt(p->fd, IPPROTO_IP, IP_TOS, &p->opt.tos, sizeof(p->opt.tos)) < 0)
		fprintf(stderr, "ft_ping: warning: cannot set TOS: %s\n", strerror(errno));
	if (p->opt.have_ip_timestamp) {
		memset(timestamp, 0, sizeof(timestamp));
		timestamp[0] = 68;
		timestamp[1] = sizeof(timestamp);
		timestamp[2] = 5;
		if (setsockopt(p->fd, IPPROTO_IP, IP_OPTIONS, timestamp,
			sizeof(timestamp)) < 0)
			fprintf(stderr, "ft_ping: warning: cannot set IP timestamp: %s\n",
				strerror(errno));
	}
	return 0;
}

static int send_echo(t_ping *p)
{
	size_t packet_len = 8 + p->opt.size;
	unsigned char *packet = calloc(1, packet_len);
	struct timeval now;
	ssize_t n;
	uint16_t checksum;
	size_t i;

	if (!packet) {
		fprintf(stderr, "ft_ping: unable to allocate packet buffer\n");
		return -1;
	}
	packet[0] = ICMP_ECHO;
	packet[4] = (unsigned char)(p->id >> 8);
	packet[5] = (unsigned char)p->id;
	packet[6] = (unsigned char)(p->seq >> 8);
	packet[7] = (unsigned char)p->seq;
	gettimeofday(&now, NULL);
	if (p->opt.size >= sizeof(now))
		memcpy(packet + 8, &now, sizeof(now));
	for (i = sizeof(now); i < p->opt.size; i++) {
		if (p->opt.have_pattern)
			packet[8 + i] = p->opt.pattern[(i - sizeof(now)) % p->opt.pattern_len];
		else
			packet[8 + i] = (unsigned char)i;
	}
	checksum = icmp_checksum(packet, packet_len);
	packet[2] = (unsigned char)(checksum >> 8);
	packet[3] = (unsigned char)checksum;
	n = sendto(p->fd, packet, packet_len, 0,
		(struct sockaddr *)&p->dest, sizeof(p->dest));
	free(packet);
	if (n < 0) {
		fprintf(stderr, "ft_ping: could not send packet: %s\n", strerror(errno));
		p->errors++;
		return -1;
	}
	p->sent++;
	p->seq++;
	p->last_send = now;
	return 0;
}

static int recv_reply(t_ping *p)
{
	unsigned char buf[65536];
	struct sockaddr_in from;
	socklen_t flen = sizeof(from);
	ssize_t n = recvfrom(p->fd, buf, sizeof(buf), 0,
		(struct sockaddr *)&from, &flen);
	size_t hlen;
	struct ip *ip;
	unsigned char *icmp;
	unsigned int seq;
	struct timeval now, sent;
	double rtt = 0;
	char addr[INET_ADDRSTRLEN];
	if (n < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return 0;
		fprintf(stderr, "ft_ping: could not receive packet: %s\n", strerror(errno));
		return -1;
	}
	if ((size_t)n < sizeof(struct ip) + 8)
		return 0;
	ip = (struct ip *)buf;
	hlen = (size_t)ip->ip_hl * 4;
	if (hlen < sizeof(struct ip) || (size_t)n < hlen + 8)
		return 0;
	icmp = buf + hlen;
	if (icmp[0] != ICMP_ECHOREPLY || ((uint16_t)icmp[4] << 8 | icmp[5]) != p->id) {
		if (p->opt.verbose) {
			inet_ntop(AF_INET, &from.sin_addr, addr, sizeof(addr));
			printf("From %s: ICMP type=%u code=%u\n", addr, icmp[0], icmp[1]);
		}
		return 0;
	}
	seq = (unsigned int)icmp[6] << 8 | icmp[7];
	gettimeofday(&now, NULL);
	if ((size_t)n >= hlen + 8 + sizeof(sent)) {
		memcpy(&sent, icmp + 8, sizeof(sent));
		rtt = elapsed_ms(&sent, &now);
	}
	p->received++;
	if (p->received == 1) {
		p->min = rtt;
		p->max = rtt;
	} else {
		if (rtt < p->min)
			p->min = rtt;
		if (rtt > p->max)
			p->max = rtt;
	}
	p->sum += rtt;
	p->sumsq += rtt * rtt;
	inet_ntop(AF_INET, &from.sin_addr, addr, sizeof(addr));
	if (p->opt.flood) {
		putchar('\b');
		fflush(stdout);
	} else {
		printf("%ld bytes from %s (%s): icmp_seq=%u ttl=%u time=%.3f ms\n",
			(long)((size_t)n - hlen), p->name, addr, seq,
			(unsigned int)ip->ip_ttl, rtt);
	}
	return 1;
}

static void print_stats(t_ping *p) {
	struct timeval now;
	double loss;
	double average;
	double deviation;

	gettimeofday(&now, NULL);
	printf("\n--- %s ping statistics ---\n", p->opt.host);
	loss = p->sent > 0
		? 100.0 * (p->sent - p->received) / p->sent
		: 0.0;
	printf("%lu packets transmitted, %lu packets received, %.1f%% packet loss, "
		"time %.0fms\n", p->sent, p->received, loss,
		elapsed_ms(&p->started, &now));
	if (p->received) {
		average = p->sum / p->received;
		deviation = p->sumsq / p->received - average * average;
		if (deviation < 0)
			deviation = 0;
		printf("rtt min/avg/max/mdev = %.3f/%.3f/%.3f/%.3f ms\n",
			p->min, average, p->max, sqrt(deviation));
	}
}


static void ping_loop(t_ping *p)
{
	struct timeval now;
	struct timeval wait;
	fd_set rfds;
	double since_send;
	double running;
	unsigned int initial = p->opt.preload;
	int ready;

	gettimeofday(&p->started, NULL);
	while (g_running) {
		gettimeofday(&now, NULL);
		running = elapsed_sec(&p->started, &now);
		if (p->opt.have_deadline && running >= p->opt.deadline)
			break;

		if (initial > 0) {
			send_echo(p);
			initial--;
		} else if (p->sent == 0
			|| elapsed_sec(&p->last_send, &now) >= p->opt.interval) {
			send_echo(p);
		}

		gettimeofday(&now, NULL);
		since_send = elapsed_sec(&p->last_send, &now);
		wait.tv_sec = 0;
		if (initial > 0) {
			wait.tv_usec = 0;
		} else if (p->sent == 0) {
			wait.tv_usec = 100000;
		} else {
			double remain = p->opt.interval - since_send;
			double until_deadline;

			if (p->opt.have_timeout
				&& p->opt.timeout - since_send < remain)
				remain = p->opt.timeout - since_send;
			until_deadline = p->opt.deadline
				- elapsed_sec(&p->started, &now);
			if (p->opt.have_deadline && until_deadline < remain)
				remain = until_deadline;
			if (remain < 0.001)
				remain = 0.001;
			wait.tv_sec = (time_t)remain;
			wait.tv_usec = (suseconds_t)((remain - wait.tv_sec) * 1000000.0);
		}
		FD_ZERO(&rfds);
		FD_SET(p->fd, &rfds);
		ready = select(p->fd + 1, &rfds, NULL, NULL, &wait);
		if (ready > 0) {
			recv_reply(p);
		} else if (ready < 0 && errno != EINTR) {
			fprintf(stderr, "ft_ping: waiting for packets failed: %s\n",
				strerror(errno));
			break;
		}
		gettimeofday(&now, NULL);
		if (p->opt.have_timeout && p->sent
			&& elapsed_sec(&p->last_send, &now) >= p->opt.timeout
			&& elapsed_sec(&p->last_send, &now) < p->opt.interval)
			continue;
	}
	print_stats(p);
}

int main(int ac, char **av) {
	t_ping p;
	const char *option_error = NULL;
	memset(&p, 0, sizeof(p));
	if (parse_options(ac, av, &p.opt, &option_error) < 0) {
		fprintf(stderr, "ft_ping: %s.\nTry 'ft_ping --help' for usage.\n",
			option_error ? option_error : "invalid arguments");
		return 2;
	}
	if (resolve_host(&p) < 0 || open_socket(&p) < 0)
		return 2;
	p.id = (uint16_t)getpid();
	if (p.opt.flood && geteuid() != 0) {
		fprintf(stderr, "ft_ping: flood mode requires root privileges\n");
		close(p.fd);
		return 2;
	}
	if (p.opt.flood)
		p.opt.interval = 0.01;
	printf("PING %s (%s) %zu(%zu) bytes of data.\n", p.opt.host, p.address, p.opt.size, p.opt.size + 28);
	fflush(stdout);
	signal(SIGINT, on_sigint);
	ping_loop(&p);
	close(p.fd);
	return 0;
}

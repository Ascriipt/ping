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

/*
 * Current time from a clock that never jumps. gettimeofday() follows the
 * wall clock, which NTP or a VM's time sync can step backwards, delaying
 * the next send (and making an RTT negative).
 */
static void get_time(struct timeval *tv)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / 1000;
}

static double elapsed_ms(const struct timeval *a, const struct timeval *b) {
	return (b->tv_sec - a->tv_sec) * 1000.0
		+ (b->tv_usec - a->tv_usec) / 1000.0;
}

static double elapsed_sec(const struct timeval *a, const struct timeval *b) {
	return elapsed_ms(a, b) / 1000.0;
}

/* Replies carry a send time (and so a time= field) only if it fits in the data. */
static bool has_timing(const t_ping *p)
{
	return p->opt.size >= sizeof(struct timeval);
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

static void resolve_host(t_ping *p)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_RAW;
	hints.ai_flags = AI_CANONNAME;   /* follow aliases: www.github.com -> github.com */
	if (getaddrinfo(p->opt.host, NULL, &hints, &res) != 0 || !res)
		die("unknown host");
	memcpy(&p->dest, res->ai_addr, sizeof(p->dest));
	snprintf(p->hostname, sizeof(p->hostname), "%s",
		res->ai_canonname ? res->ai_canonname : p->opt.host);
	freeaddrinfo(res);
	inet_ntop(AF_INET, &p->dest.sin_addr, p->address, sizeof(p->address));
}

static void set_ip_timestamp(t_ping *p)
{
	unsigned char opt[40];
	size_t len = p->opt.ip_timestamp == TS_ADDR ? 36 : 40;

	memset(opt, 0, sizeof(opt));
	opt[0] = IPOPT_TIMESTAMP;
	opt[1] = (unsigned char)len;
	opt[2] = 5;   /* pointer to the first free slot (1-based) */
	opt[3] = p->opt.ip_timestamp == TS_ADDR ? 1 : 0;
	if (setsockopt(p->fd, IPPROTO_IP, IP_OPTIONS, opt, len) < 0)
		die("setsockopt(IP_OPTIONS): %s", strerror(errno));
}

static void open_socket(t_ping *p)
{
	int one = 1;

	p->fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (p->fd < 0)
		die("cannot open ICMP socket: %s", strerror(errno));
	setsockopt(p->fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	if (p->opt.bypass
		&& setsockopt(p->fd, SOL_SOCKET, SO_DONTROUTE, &one, sizeof(one)) < 0)
		die("setsockopt(SO_DONTROUTE): %s", strerror(errno));
	if (p->opt.have_ttl
		&& setsockopt(p->fd, IPPROTO_IP, IP_TTL, &p->opt.ttl, sizeof(p->opt.ttl)) < 0)
		die("setsockopt(IP_TTL): %s", strerror(errno));
	if (p->opt.have_tos
		&& setsockopt(p->fd, IPPROTO_IP, IP_TOS, &p->opt.tos, sizeof(p->opt.tos)) < 0)
		die("setsockopt(IP_TOS): %s", strerror(errno));
	if (p->opt.ip_timestamp != TS_NONE)
		set_ip_timestamp(p);
}

static void send_echo(t_ping *p)
{
	size_t packet_len = 8 + p->opt.size;
	unsigned char *packet = calloc(1, packet_len);
	size_t start = has_timing(p) ? sizeof(struct timeval) : 0;
	struct timeval now;
	uint16_t checksum;
	size_t i;

	if (!packet)
		die("unable to allocate packet buffer");
	packet[0] = ICMP_ECHO;
	packet[4] = (unsigned char)(p->id >> 8);
	packet[5] = (unsigned char)p->id;
	packet[6] = (unsigned char)(p->seq >> 8);
	packet[7] = (unsigned char)p->seq;
	get_time(&now);
	if (has_timing(p))
		memcpy(packet + 8, &now, sizeof(now));
	for (i = start; i < p->opt.size; i++) {
		if (p->opt.pattern_len)
			packet[8 + i] = p->opt.pattern[(i - start) % p->opt.pattern_len];
		else
			packet[8 + i] = (unsigned char)i;
	}
	checksum = icmp_checksum(packet, packet_len);
	packet[2] = (unsigned char)(checksum >> 8);
	packet[3] = (unsigned char)checksum;
	if (sendto(p->fd, packet, packet_len, 0,
		(struct sockaddr *)&p->dest, sizeof(p->dest)) < 0) {
		free(packet);
		die("sending packet: %s", strerror(errno));
	}
	free(packet);
	/* Flood mode: one dot per packet sent after the first, erased by its reply. */
	if (p->opt.flood && p->sent > 0) {
		putchar('.');
		fflush(stdout);
	}
	p->sent++;
	p->seq++;
	p->last_send = now;
}

static const char *icmp_error_text(unsigned int type, unsigned int code)
{
	static const char *unreach[] = {
		"Destination Net Unreachable",
		"Destination Host Unreachable",
		"Destination Protocol Unreachable",
		"Destination Port Unreachable",
		"Fragmentation needed and DF set",
		"Source Route Failed",
		"Network Unknown",
		"Host Unknown",
		"Host Isolated",
		"Destination Network Prohibited",
		"Destination Host Prohibited",
		"Network Unreachable For Type Of Service",
		"Host Unreachable For Type Of Service",
		"Packet Filtered",
		"Precedence Violation",
		"Precedence Cutoff",
	};
	static const char *redirect[] = {
		"Redirect Network",
		"Redirect Host",
		"Redirect Type of Service and Network",
		"Redirect Type of Service and Host",
	};

	switch (type) {
	case ICMP_DEST_UNREACH:
		if (code < sizeof(unreach) / sizeof(*unreach))
			return unreach[code];
		return "Destination Unreachable";
	case ICMP_SOURCE_QUENCH:
		return "Source Quench";
	case ICMP_REDIRECT:
		if (code < sizeof(redirect) / sizeof(*redirect))
			return redirect[code];
		return "Redirect";
	case ICMP_TIME_EXCEEDED:
		return code == 0 ? "Time to live exceeded"
			: "Frag reassembly time exceeded";
	case ICMP_PARAMETERPROB:
		return "Parameter problem";
	}
	return NULL;
}

/* -v: dump the header of the packet we sent, as quoted inside an ICMP error. */
static void print_ip_header_dump(const struct ip *ip)
{
	const unsigned char *bytes = (const unsigned char *)ip;
	size_t hlen = (size_t)ip->ip_hl * 4;
	unsigned int off = ntohs(ip->ip_off);
	size_t i;

	printf("IP Hdr Dump:\n ");
	for (i = 0; i + 1 < hlen; i += 2)
		printf("%02x%02x ", bytes[i], bytes[i + 1]);
	printf("\n");
	printf("Vr HL TOS  Len   ID Flg  off TTL Pro  cks      Src\tDst\tData\n");
	printf(" %1x  %1x  %02x %04x %04x", ip->ip_v, ip->ip_hl, ip->ip_tos,
		ntohs(ip->ip_len), ntohs(ip->ip_id));
	printf("   %1x %04x", (off & 0xe000) >> 13, off & 0x1fff);
	printf("  %02x  %02x %04x", ip->ip_ttl, ip->ip_p, ntohs(ip->ip_sum));
	printf(" %s ", inet_ntoa(ip->ip_src));
	printf(" %s ", inet_ntoa(ip->ip_dst));
	printf("\n");
}

/*
 * An ICMP error quotes the IP header and first 8 bytes of the packet that
 * caused it. Report it only if that packet was one of our echo requests.
 */
static void handle_icmp_error(t_ping *p, const unsigned char *icmp,
	size_t icmp_len, const char *from)
{
	const struct ip *inner;
	const unsigned char *inner_icmp;
	size_t inner_hlen;
	const char *text = icmp_error_text(icmp[0], icmp[1]);

	if (!text || icmp_len < 8 + sizeof(struct ip) + 8)
		return;
	inner = (const struct ip *)(icmp + 8);
	inner_hlen = (size_t)inner->ip_hl * 4;
	if (inner->ip_p != IPPROTO_ICMP || icmp_len < 8 + inner_hlen + 8)
		return;
	inner_icmp = icmp + 8 + inner_hlen;
	if (inner_icmp[0] != ICMP_ECHO
		|| ((uint16_t)inner_icmp[4] << 8 | inner_icmp[5]) != p->id)
		return;
	printf("%zu bytes from %s: %s\n", icmp_len, from, text);
	if (p->opt.verbose) {
		print_ip_header_dump(inner);
		printf("ICMP: type %u, code %u, size %u, id 0x%04x, seq 0x%04x\n",
			inner_icmp[0], inner_icmp[1],
			(unsigned int)(ntohs(inner->ip_len) - inner_hlen),
			(unsigned int)(inner_icmp[4] << 8 | inner_icmp[5]),
			(unsigned int)(inner_icmp[6] << 8 | inner_icmp[7]));
	}
}

/* --ip-timestamp: print the timestamps the hops wrote in the reply's IP options. */
static void print_timestamp_option(const t_ping *p, const unsigned char *opt,
	size_t len)
{
	size_t step = (opt[3] & 0x0f) == 0 ? 4 : 8;
	size_t end = opt[2] > 0 ? (size_t)opt[2] - 1 : 0;
	size_t i;
	char addr[INET_ADDRSTRLEN];
	uint32_t ms;

	if (end > len)
		end = len;
	for (i = 4; i + step <= end; i += step) {
		printf("%s\t", i == 4 ? "TS:" : "");
		if (step == 8) {
			inet_ntop(AF_INET, opt + i, addr, sizeof(addr));
			printf("%s\t", addr);
		}
		memcpy(&ms, opt + i + step - 4, sizeof(ms));
		ms = ntohl(ms);
		printf("%u ms", ms);
		if (p->opt.verbose)
			printf(" = 0x%08x", ms);
		printf("\n");
	}
	printf("\n");
}

static void print_ip_options(const t_ping *p, const struct ip *ip)
{
	const unsigned char *opt = (const unsigned char *)ip + sizeof(struct ip);
	size_t optlen = (size_t)ip->ip_hl * 4 - sizeof(struct ip);
	size_t i = 0;

	while (i < optlen) {
		if (opt[i] == IPOPT_EOL)
			break;
		if (opt[i] == IPOPT_NOP) {
			i++;
			continue;
		}
		if (i + 1 >= optlen || opt[i + 1] < 2 || i + opt[i + 1] > optlen)
			break;
		if (opt[i] == IPOPT_TIMESTAMP && opt[i + 1] >= 4)
			print_timestamp_option(p, opt + i, opt[i + 1]);
		i += opt[i + 1];
	}
}

static void handle_echo_reply(t_ping *p, const struct ip *ip,
	const unsigned char *icmp, size_t icmp_len, const char *from)
{
	unsigned int seq = (unsigned int)icmp[6] << 8 | icmp[7];
	bool timing = has_timing(p) && icmp_len >= 8 + sizeof(struct timeval);
	struct timeval now, sent;
	double rtt = 0;

	p->received++;
	if (timing) {
		get_time(&now);
		memcpy(&sent, icmp + 8, sizeof(sent));
		rtt = elapsed_ms(&sent, &now);
		if (p->timed == 0 || rtt < p->min)
			p->min = rtt;
		if (p->timed == 0 || rtt > p->max)
			p->max = rtt;
		p->timed++;
		p->sum += rtt;
		p->sumsq += rtt * rtt;
	}
	if (p->opt.flood) {
		putchar('\b');
		fflush(stdout);
		return;
	}
	printf("%zu bytes from %s: icmp_seq=%u ttl=%u", icmp_len, from, seq,
		(unsigned int)ip->ip_ttl);
	if (timing)
		printf(" time=%.3f ms", rtt);
	printf("\n");
	print_ip_options(p, ip);
}

static void recv_reply(t_ping *p)
{
	unsigned char buf[65536];
	struct sockaddr_in from;
	socklen_t flen = sizeof(from);
	char addr[INET_ADDRSTRLEN];
	const struct ip *ip;
	const unsigned char *icmp;
	size_t hlen;
	ssize_t n;

	n = recvfrom(p->fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
	if (n < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return;
		die("receiving packet: %s", strerror(errno));
	}
	ip = (const struct ip *)buf;
	if ((size_t)n < sizeof(struct ip))
		return;
	hlen = (size_t)ip->ip_hl * 4;
	if (hlen < sizeof(struct ip) || (size_t)n < hlen + 8)
		return;
	icmp = buf + hlen;
	inet_ntop(AF_INET, &from.sin_addr, addr, sizeof(addr));
	if (icmp[0] == ICMP_ECHOREPLY) {
		if (((uint16_t)icmp[4] << 8 | icmp[5]) == p->id)
			handle_echo_reply(p, ip, icmp, (size_t)n - hlen, addr);
	} else if (icmp[0] != ICMP_ECHO) {
		/* Our own requests also show up on loopback: ignore them. */
		handle_icmp_error(p, icmp, (size_t)n - hlen, addr);
	}
}

static void print_stats(const t_ping *p) {
	double average;
	double deviation;

	printf("--- %s ping statistics ---\n", p->hostname);
	printf("%lu packets transmitted, %lu packets received, %lu%% packet loss\n",
		p->sent, p->received,
		p->sent > 0 ? (p->sent - p->received) * 100 / p->sent : 0);
	if (p->timed) {
		average = p->sum / p->timed;
		deviation = p->sumsq / p->timed - average * average;
		if (deviation < 0)
			deviation = 0;
		printf("round-trip min/avg/max/stddev = %.3f/%.3f/%.3f/%.3f ms\n",
			p->min, average, p->max, sqrt(deviation));
	}
}

static void add_seconds(struct timeval *tv, double sec)
{
	long usec = (long)(sec * 1000000.0);

	tv->tv_sec += usec / 1000000;
	tv->tv_usec += usec % 1000000;
	if (tv->tv_usec >= 1000000) {
		tv->tv_sec++;
		tv->tv_usec -= 1000000;
	}
}

/*
 * Send the preload burst and the first packet right away, then one packet
 * per interval. With -w, stop right after the send that reaches the deadline.
 */
static void ping_loop(t_ping *p)
{
	struct timeval now;
	struct timeval next;
	struct timeval wait;
	fd_set rfds;
	unsigned int i;
	int ready;

	get_time(&p->started);
	for (i = 0; i < p->opt.preload; i++)
		send_echo(p);
	send_echo(p);
	next = p->started;
	add_seconds(&next, p->opt.interval);
	while (g_running) {
		get_time(&now);
		if (!timercmp(&now, &next, <)) {
			send_echo(p);
			add_seconds(&next, p->opt.interval);
			if (timercmp(&next, &now, <)) {
				next = now;
				add_seconds(&next, p->opt.interval);
			}
			if (p->opt.deadline
				&& elapsed_sec(&p->started, &now) >= (double)p->opt.deadline)
				break;
			continue;
		}
		timersub(&next, &now, &wait);
		FD_ZERO(&rfds);
		FD_SET(p->fd, &rfds);
		ready = select(p->fd + 1, &rfds, NULL, NULL, &wait);
		if (ready > 0)
			recv_reply(p);
		else if (ready < 0 && errno != EINTR)
			die("select: %s", strerror(errno));
	}
}

int main(int ac, char **av) {
	t_ping p;

	memset(&p, 0, sizeof(p));
	parse_options(ac, av, &p.opt);
	resolve_host(&p);
	open_socket(&p);
	p.id = (uint16_t)getpid();
	printf("PING %s (%s): %zu data bytes", p.hostname, p.address, p.opt.size);
	if (p.opt.verbose)
		printf(", id 0x%04x = %u", p.id, p.id);
	printf("\n");
	fflush(stdout);
	signal(SIGINT, on_sigint);
	ping_loop(&p);
	print_stats(&p);
	close(p.fd);
	return p.received ? 0 : 1;
}

#pragma once

#ifndef _DEFAULT_SOURCE
# define _DEFAULT_SOURCE   /* needed for NI_MAXHOST in <netdb.h> */
#endif

#include <stdbool.h>      /* bool */
#include <stddef.h>       /* size_t */
#include <stdint.h>       /* uint16_t */
#include <sys/time.h>     /* struct timeval */
#include <netinet/in.h>   /* struct sockaddr_in, INET_ADDRSTRLEN */
#include <netdb.h>        /* NI_MAXHOST */

enum e_timestamp {
	TS_NONE = 0,
	TS_ONLY,   /* --ip-timestamp=tsonly */
	TS_ADDR,   /* --ip-timestamp=tsaddr */
};

typedef struct s_options {
	bool verbose;
	bool flood;
	bool numeric;               /* accepted; no reverse DNS is done anyway */
	bool bypass;
	bool have_ttl;
	bool have_tos;
	enum e_timestamp ip_timestamp;
	unsigned int preload;
	unsigned long deadline;     /* -w, seconds, 0 = none */
	unsigned long linger;       /* -W, seconds, 0 = default */
	double interval;
	size_t size;
	int ttl;
	int tos;
	unsigned char pattern[16];
	size_t pattern_len;         /* 0 = no pattern */
	const char *host;
} t_options;

typedef struct s_ping {
	int fd;
	struct sockaddr_in dest;
	char address[INET_ADDRSTRLEN];
	t_options opt;
	uint16_t id;
	uint16_t seq;
	unsigned long sent;
	unsigned long received;
	unsigned long timed;        /* replies that carried a send time */
	double min;
	double max;
	double sum;
	double sumsq;
	struct timeval started;
	struct timeval last_send;
} t_ping;
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

typedef struct s_options {
	bool verbose;
	bool flood;
	bool numeric;
	bool bypass;
	bool have_deadline;
	bool have_timeout;
	bool have_ttl;
	bool have_tos;
	bool have_pattern;
	bool have_ip_timestamp;
	unsigned int preload;
	double deadline;
	double timeout;
	double interval;
	size_t size;
	int ttl;
	int tos;
	unsigned char pattern[16];
	size_t pattern_len;
	const char *host;
} t_options;

typedef struct s_ping {
	int fd;
	struct sockaddr_in dest;
	char address[INET_ADDRSTRLEN];
	char name[NI_MAXHOST];
	t_options opt;
	uint16_t id;
	uint16_t seq;
	unsigned long sent;
	unsigned long received;
	unsigned long errors;
	double min;
	double max;
	double sum;
	double sumsq;
	struct timeval started;
	struct timeval last_send;
} t_ping;
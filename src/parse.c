#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include "parse.h"

int parse_unsigned(const char *str, unsigned long max, unsigned long *out) {
	char *end;
	unsigned long n;
	if (!str || !*str || *str == '-')
		return -1;
	errno = 0;
	n = strtoul(str, &end, 10);
	if (errno || *end || n > max)
		return -1;
	*out = n;
	return 0;
}

int parse_double(const char *str, double *out) {
	char *end;
	double n;
	if (!str || !*str || *str == '-')
		return -1;
	errno = 0;
	n = strtod(str, &end);
	if (errno || *end || n <= 0.0)
		return -1;
	*out = n;
	return 0;
}

static void usage(FILE *out) {
	fputs("Usage: ft_ping [OPTION...] HOST\n"
		"Send ICMP ECHO_REQUEST packets to network hosts.\n\n"
		"  -?, --help             give this help list\n"
		"  -v, --verbose          verbose output\n"
		"  -V, --version          print version and exit\n"
		"  -f, --flood             send packets as quickly as possible\n"
		"  -l, --preload=COUNT     send COUNT packets before the regular interval\n"
		"  -n, --numeric           numeric output only\n"
		"  -w, --deadline=SEC      stop after SEC seconds\n"
		"  -W, --timeout=SEC       wait SEC seconds for each reply\n"
		"  -p, --pattern=HEX       fill payload with a hexadecimal pattern\n"
		"  -r, --ignore-routing    bypass normal routing tables\n"
		"  -s, --size=SIZE         payload size in bytes\n"
		"  -T, --tos=NUM           set IP type of service\n"
		"      --ttl=NUM           set IP time to live\n"
		"      --ip-timestamp      request an IP timestamp option\n"
		"      --usage             give a short usage message\n"
		"      --echo              send an echo request (compatibility option)\n\n"
		"Examples:\n"
		"  ft_ping example.com\n"
		"  ft_ping --deadline=5 --size=120 192.0.2.1\n",
		out);
}

int parse_pattern(const char *s, t_options *o) {
	size_t n = strlen(s), i;
	if (n == 0 || (n & 1) || n > sizeof(o->pattern) * 2) return -1;
	for (i = 0; i < n / 2; i++) {
		unsigned int byte;
		if (sscanf(s + i * 2, "%2x", &byte) != 1) return -1;
		if (!((s[i*2] >= '0' && s[i*2] <= '9') || (s[i*2] >= 'a' && s[i*2] <= 'f') || (s[i*2] >= 'A' && s[i*2] <= 'F')) ||
			!((s[i*2+1] >= '0' && s[i*2+1] <= '9') || (s[i*2+1] >= 'a' && s[i*2+1] <= 'f') || (s[i*2+1] >= 'A' && s[i*2+1] <= 'F'))) return -1;
		o->pattern[i] = (unsigned char)byte;
	}
	o->pattern_len = n / 2;
	o->have_pattern = true;
	return 0;
}

int option_value(int *i, int ac, char **av, const char *arg, const char *name, const char **value) {
	size_t n = strlen(name);
	if (strncmp(arg, name, n) != 0) return 0;
	if (arg[n] == '=') { *value = arg + n + 1; return 1; }
	if (arg[n] != '\0') return 0;
	if (*i + 1 >= ac) return -1;
	*value = av[++*i];
	return 1;
}

int read_option_value(int *index, int argc, char **argv,
	const char *arg, const char *long_name, const char *short_name,
	const char **value)
{
	int result = option_value(index, argc, argv, arg, long_name, value);
	if (result != 0)
		return result;
	if (short_name && strcmp(arg, short_name) == 0) {
		if (*index + 1 >= argc)
			return -1;
		*value = argv[++*index];
		return 1;
	}
	if (short_name && arg[0] == '-' && arg[1] == short_name[1]
		&& arg[2] != '\0') {
		*value = arg + 2;
		return 1;
	}
	return 0;
}

int parse_options(int argc, char **argv, t_options *o,
	const char **error)
{
	int i;
	memset(o, 0, sizeof(*o));
	o->size = DEFAULT_SIZE;
	o->interval = DEFAULT_INTERVAL;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *value = NULL;
		unsigned long number;
		int matched;

		if (strcmp(arg, "--help") == 0 || strcmp(arg, "-?") == 0) {
			usage(stdout);
			exit(0);
		}
		if (strcmp(arg, "--usage") == 0) {
			puts("Usage: ft_ping [OPTION...] HOST");
			exit(0);
		}
		if (strcmp(arg, "--version") == 0 || strcmp(arg, "-V") == 0) {
			puts("ft_ping (42 ft_ping) 1.0");
			exit(0);
		}
		if (strcmp(arg, "--echo") == 0)
			continue;
		if (strcmp(arg, "--verbose") == 0 || strcmp(arg, "-v") == 0) {
			o->verbose = true;
			continue;
		}
		if (strcmp(arg, "--flood") == 0 || strcmp(arg, "-f") == 0) {
			o->flood = true;
			o->interval = 0.01;
			continue;
		}
		if (strcmp(arg, "--numeric") == 0 || strcmp(arg, "-n") == 0) {
			o->numeric = true;
			continue;
		}
		if (strcmp(arg, "--ignore-routing") == 0 || strcmp(arg, "-r") == 0) {
			o->bypass = true;
			continue;
		}
		if (strcmp(arg, "--ip-timestamp") == 0) {
			o->have_ip_timestamp = true;
			continue;
		}

		matched = read_option_value(&i, argc, argv, arg, "--preload", "-l", &value);
		if (matched != 0) {
			if (matched < 0 || parse_unsigned(value, 1000000, &number) < 0) {
				*error = "--preload expects a whole number between 0 and 1000000";
				return -1;
			}
			o->preload = (unsigned int)number;
			continue;
		}

		matched = read_option_value(&i, argc, argv, arg, "--deadline", "-w", &value);
		if (matched != 0) {
			if (matched < 0 || parse_double(value, &o->deadline) < 0) {
				*error = "--deadline expects a number greater than zero";
				return -1;
			}
			o->have_deadline = true;
			continue;
		}

		matched = read_option_value(&i, argc, argv, arg, "--timeout", "-W", &value);
		if (matched != 0) {
			if (matched < 0 || parse_double(value, &o->timeout) < 0) {
				*error = "--timeout expects a number greater than zero";
				return -1;
			}
			o->have_timeout = true;
			continue;
		}

		matched = read_option_value(&i, argc, argv, arg, "--size", "-s", &value);
		if (matched != 0) {
			if (matched < 0 || parse_unsigned(value, MAX_SIZE, &number) < 0) {
				*error = "--size expects a whole number between 0 and 65507";
				return -1;
			}
			o->size = (size_t)number;
			continue;
		}

		matched = read_option_value(&i, argc, argv, arg, "--pattern", "-p", &value);
		if (matched != 0) {
			if (matched < 0 || parse_pattern(value, o) < 0) {
				*error = "--pattern expects an even number of hexadecimal digits (up to 32)";
				return -1;
			}
			continue;
		}

		matched = read_option_value(&i, argc, argv, arg, "--ttl", NULL, &value);
		if (matched != 0) {
			if (matched < 0 || parse_unsigned(value, 255, &number) < 0 || number == 0) {
				*error = "--ttl expects a whole number between 1 and 255";
				return -1;
			}
			o->ttl = (int)number;
			o->have_ttl = true;
			continue;
		}

		matched = read_option_value(&i, argc, argv, arg, "--tos", "-T", &value);
		if (matched != 0) {
			if (matched < 0 || parse_unsigned(value, 255, &number) < 0) {
				*error = "--tos expects a whole number between 0 and 255";
				return -1;
			}
			o->tos = (int)number;
			o->have_tos = true;
			continue;
		}

		if (arg[0] == '-') {
			*error = "unknown option";
			return -1;
		}
		if (o->host) {
			*error = "please provide only one host";
			return -1;
		}
		o->host = arg;
	}
	if (!o->host) {
		*error = "please provide a host name or IPv4 address";
		return -1;
	}
	return 0;
}

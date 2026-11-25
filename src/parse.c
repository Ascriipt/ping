#define _GNU_SOURCE   /* program_invocation_short_name */
#include <argp.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sysexits.h>

#include "parse.h"

/* Keys for options that only have a long name. */
enum {
	OPT_ECHO = 256,
	OPT_TTL,
	OPT_IP_TIMESTAMP,
	OPT_USAGE,
};

/* Help groups, same layout as inetutils ping. */
enum {
	GRP_TYPES = 10,
	GRP_ALL = 20,
	GRP_ECHO = 30,
};

#define VERSION_TEXT "ft_ping (42 ft_ping) 1.0"

static const struct argp_option g_options[] = {
	{ NULL, 0, NULL, 0, "Options controlling ICMP request types:", GRP_TYPES },
	{ "echo", OPT_ECHO, NULL, 0, "send ICMP_ECHO packets (default)", GRP_TYPES },

	{ NULL, 0, NULL, 0, "Options valid for all request types:", GRP_ALL },
	{ "numeric", 'n', NULL, 0, "do not resolve host addresses", GRP_ALL + 1 },
	{ "ignore-routing", 'r', NULL, 0,
		"send directly to a host on an attached network", GRP_ALL + 1 },
	{ "ttl", OPT_TTL, "N", 0, "specify N as time-to-live", GRP_ALL + 1 },
	{ "tos", 'T', "NUM", 0, "set type of service (TOS) to NUM", GRP_ALL + 1 },
	{ "verbose", 'v', NULL, 0, "verbose output", GRP_ALL + 1 },
	{ "timeout", 'w', "N", 0, "stop after N seconds", GRP_ALL + 1 },
	{ "linger", 'W', "N", 0, "number of seconds to wait for response", GRP_ALL + 1 },

	{ NULL, 0, NULL, 0, "Options valid for --echo requests:", GRP_ECHO },
	{ "flood", 'f', NULL, 0, "flood ping (root only)", GRP_ECHO + 1 },
	{ "ip-timestamp", OPT_IP_TIMESTAMP, "FLAG", 0,
		"IP timestamp of type FLAG, which is one of \"tsonly\" and \"tsaddr\"",
		GRP_ECHO + 1 },
	{ "preload", 'l', "NUMBER", 0,
		"send NUMBER packets as fast as possible before falling into normal "
		"mode of behavior (root only)", GRP_ECHO + 1 },
	{ "pattern", 'p', "PATTERN", 0, "fill ICMP packet with given pattern (hex)",
		GRP_ECHO + 1 },
	{ "size", 's', "NUMBER", 0, "send NUMBER data octets", GRP_ECHO + 1 },

	/* argp's own help entries are capitalized; inetutils' are not. */
	{ "help", '?', NULL, 0, "give this help list", -1 },
	{ "usage", OPT_USAGE, NULL, 0, "give a short usage message", -1 },
	{ "version", 'V', NULL, 0, "print program version", -1 },
	{ NULL, 0, NULL, 0, NULL, 0 },
};

/* Print "ft_ping: <message>" on stderr and exit with status 1. */
void die(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", program_invocation_short_name);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(EXIT_FAILURE);
}

/*
 * Number in any C base (strtoul base 0, so 0x10 and 010 work).
 * A negative number wraps to a huge value, so it is reported as "too big".
 */
unsigned long parse_number(const char *arg, unsigned long min, unsigned long max)
{
	char *end;
	unsigned long n;

	errno = 0;
	n = strtoul(arg, &end, 0);
	if (*end)
		die("invalid value (`%s' near `%s')", arg, end);
	if (errno == ERANGE || n > max)
		die("option value too big: %s", arg);
	if (n < min)
		die("option value too small: %s", arg);
	return n;
}

/* Up to 16 bytes, read two hex digits at a time. */
void parse_pattern(const char *arg, t_options *o)
{
	const char *p = arg;
	unsigned int byte;
	int used;

	o->pattern_len = 0;
	while (*p && o->pattern_len < sizeof(o->pattern)) {
		if (sscanf(p, "%2x%n", &byte, &used) != 1)
			die("error in pattern near %s", p);
		o->pattern[o->pattern_len++] = (unsigned char)byte;
		p += used;
	}
}

static void parse_preload(const char *arg, t_options *o)
{
	char *end;
	unsigned long n;

	errno = 0;
	n = strtoul(arg, &end, 0);
	if (*end || errno == ERANGE || n > INT_MAX)
		die("invalid preload value (%s)", arg);
	o->preload = (unsigned int)n;
}

static void parse_ip_timestamp(const char *arg, t_options *o)
{
	if (strcasecmp(arg, "tsonly") == 0)
		o->ip_timestamp = TS_ONLY;
	else if (strcasecmp(arg, "tsaddr") == 0)
		o->ip_timestamp = TS_ADDR;
	else
		die("unsupported timestamp type: %s", arg);
}

/* argp writes its "Try ... --help" hint here so the quotes can be fixed. */
static FILE *g_argp_errors;

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	t_options *o = state->input;

	switch (key) {
	case ARGP_KEY_INIT:
		state->err_stream = g_argp_errors;
		break;
	case OPT_ECHO:
		break;
	case 'n':
		o->numeric = true;
		break;
	case 'r':
		o->bypass = true;
		break;
	case OPT_TTL:
		o->ttl = (int)parse_number(arg, 1, 255);
		o->have_ttl = true;
		break;
	case 'T':
		o->tos = (int)parse_number(arg, 0, 255);
		o->have_tos = true;
		break;
	case 'v':
		o->verbose = true;
		break;
	case 'w':
		o->deadline = parse_number(arg, 1, INT_MAX);
		break;
	case 'W':
		o->linger = parse_number(arg, 1, INT_MAX);
		break;
	case 'f':
		o->flood = true;
		break;
	case OPT_IP_TIMESTAMP:
		parse_ip_timestamp(arg, o);
		break;
	case 'l':
		parse_preload(arg, o);
		break;
	case 'p':
		parse_pattern(arg, o);
		break;
	case 's':
		o->size = parse_number(arg, 0, MAX_SIZE);
		break;
	case '?':
		argp_state_help(state, stdout, ARGP_HELP_STD_HELP);
		exit(EXIT_SUCCESS);
	case OPT_USAGE:
		argp_state_help(state, stdout, ARGP_HELP_USAGE);
		exit(EXIT_SUCCESS);
	case 'V':
		puts(VERSION_TEXT);
		exit(EXIT_SUCCESS);
	case ARGP_KEY_ARG:
		if (o->host) {
			argp_error(state, "only one host is supported");
			return EINVAL;
		}
		o->host = arg;
		break;
	case ARGP_KEY_NO_ARGS:
		argp_error(state, "missing host operand");
		return EINVAL;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static const struct argp g_argp = {
	g_options, parse_opt, "HOST",
	"Send ICMP ECHO_REQUEST packets to network hosts."
	"\vOptions marked with (root only) are available only to superuser.",
	NULL, NULL, NULL,
};

/*
 * Usage errors print "ft_ping: <problem>" then a "Try ..." hint and exit 64.
 * glibc quotes the hint as `ft_ping --help', inetutils as 'ft_ping --help'.
 */
void parse_options(int argc, char **argv, t_options *o)
{
	char *errors = NULL;
	size_t errors_len = 0;
	error_t status;
	char *c;

	memset(o, 0, sizeof(*o));
	o->size = DEFAULT_SIZE;
	o->interval = DEFAULT_INTERVAL;
	g_argp_errors = open_memstream(&errors, &errors_len);
	if (!g_argp_errors)
		die("%s", strerror(errno));
	/* getopt prints argv[0] as typed: show "ft_ping", not "./ft_ping". */
	argv[0] = program_invocation_short_name;
	status = argp_parse(&g_argp, argc, argv, ARGP_NO_HELP | ARGP_NO_EXIT, NULL, o);
	fclose(g_argp_errors);
	if (status != 0) {
		for (c = errors; c && *c; c++)
			if (*c == '`')
				*c = '\'';
		if (errors)
			fputs(errors, stderr);
		free(errors);
		exit(EX_USAGE);
	}
	free(errors);
	if (o->flood)
		o->interval = FLOOD_INTERVAL;
}

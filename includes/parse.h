#pragma once

#include "class.h"   /* t_options */

#define DEFAULT_SIZE 56
#define MAX_SIZE 65399
#define DEFAULT_INTERVAL 1.0
#define FLOOD_INTERVAL 0.01

void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
unsigned long parse_number(const char *arg, unsigned long min, unsigned long max);
void parse_pattern(const char *arg, t_options *o);
void parse_options(int argc, char **argv, t_options *o);

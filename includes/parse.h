#pragma once

#include "class.h"   /* t_options */

int parse_double(const char *str, double *out);
int parse_unsigned(const char *str, unsigned long max, unsigned long *out);

int parse_pattern(const char *s, t_options *o);
int parse_options(int argc, char **argv, t_options *o, const char **error);

int option_value(int *i, int ac, char **av, const char *arg, const char *name, const char **value);
int read_option_value(int *index, int argc, char **argv, const char *arg, const char *long_name, const char *short_name, const char **value);
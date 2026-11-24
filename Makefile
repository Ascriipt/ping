NAME = ft_ping
CC = gcc
CFLAGS = -Wall -Wextra -Werror
CPPFLAGS = -Iincludes
LDLIBS = -lm

SRC_DIR = src
INC_DIR = includes
SRC = $(SRC_DIR)/ft_ping.c $(SRC_DIR)/parse.c
HDR = $(INC_DIR)/class.h $(INC_DIR)/parse.h

all: $(NAME)

$(NAME): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $(NAME) $(SRC) $(LDLIBS)

# Compares ft_ping with the reference ping (inetutils-2.0).
# Both need raw sockets, so the suite runs through sudo when not root.
# Override the reference binary with: make tests REAL_PING=/path/to/ping
REAL_PING ?= ping
SUDO = $(shell [ "$$(id -u)" -eq 0 ] || echo sudo)

tests: all
	$(SUDO) env REAL_PING=$(REAL_PING) ./tests/run_tests.sh ./$(NAME)

clean-objects:
	rm -f $(SRC:.c=.o)

clean:
	rm -f $(NAME)

fclean: clean clean-objects

re: fclean all

.PHONY: all clean clean-objects fclean re tests

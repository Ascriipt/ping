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

tests: all
	./ft_ping example.com
	./ft_ping --deadline=5 --size=120 192.0.2.1

clean-objects:
	rm -f $(SRC:.c=.o)

clean:
	rm -f $(NAME)

fclean: clean clean-objects

re: fclean all

.PHONY: all clean clean-objects fclean re tests

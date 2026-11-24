NAME = ft_ping
CC = gcc
CFLAGS = -Wall -Wextra -Werror
LDLIBS = -lm
SRC = ft_ping.c

all: $(NAME)

$(NAME): $(SRC)
	$(CC) $(CFLAGS) -o $(NAME) $(SRC) $(LDLIBS)

tests: all
	./ft_ping example.com
	./ft_ping --deadline=5 --size=120 192.0.2.1

clean-objects:
	rm -f $(SRC:.c=.o)

clean:
	rm -f $(NAME)

fclean: clean clean-objects

re: fclean all

.PHONY: all clean fclean re

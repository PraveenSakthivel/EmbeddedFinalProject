CC = gcc
CFLAGS = -pthread -g
#CFLAGS += -DDEBUG -Wall -Werror -g
#CFLAGS += -fsanitize=address -g
CFLAGS += -O0
SERVERDEPS =
CLIENTDEPS =
ASSTNAME = Asst3

all: server

server: server.c $(SERVERDEPS)
	$(CC) $(CFLAGS) server.c $(SERVERDEPS) -o motdServer

clean:
	rm -f motdServer

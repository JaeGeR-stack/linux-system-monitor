
CC = gcc
CFLAGS = -Wall -Wextra -g -std=c11 -pthread

COMMON_OBJS = cpu.o memory.o disk.o process.o

.PHONY: all clean run

all: monitor client server

monitor: main.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

client: client.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

server: server.o
	$(CC) $(CFLAGS) $^ -o $@ -lcjson -lm

main.o: main.c cpu.h memory.h disk.h process.h
client.o: client.c cpu.h memory.h disk.h process.h

cpu.o: cpu.c cpu.h
memory.o: memory.c memory.h
disk.o: disk.c disk.h
process.o: process.c process.h

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: monitor
	./monitor

clean:
	rm -f main.o client.o server.o $(COMMON_OBJS) monitor client server

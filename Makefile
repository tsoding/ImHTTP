CFLAGS=-Wall -Wextra -std=c11 -pedantic -ggdb

main: main.c imhttp.h
	$(CC) $(CFLAGS) -o main main.c sv.c

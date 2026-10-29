CC = cc
CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -O2
LDLIBS =

all: sws

sws: sws.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f sws

.PHONY: all clean

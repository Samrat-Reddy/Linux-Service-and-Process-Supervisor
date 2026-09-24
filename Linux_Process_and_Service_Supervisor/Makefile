CC     = gcc
CFLAGS = -Wall -Wextra -std=c11 -g

all: warden testsvc

warden: warden.c
	$(CC) $(CFLAGS) -o $@ $<

testsvc: testsvc.c
	$(CC) $(CFLAGS) -o $@ $<

run: all
	./warden services.conf

clean:
	rm -f warden testsvc warden.log

.PHONY: all run clean

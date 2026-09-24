# Warden - Linux Service and Process Supervisor

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g -Isrc
LDLIBS  = -lpthread

SRC     = src/main.c src/config.c src/service.c src/supervisor.c \
          src/health.c src/control.c src/log.c
OBJ     = $(SRC:.c=.o)

BINS    = warden wardenctl testsvc cowdemo

all: $(BINS)

warden: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

wardenctl: src/wardenctl.c
	$(CC) $(CFLAGS) -o $@ $<

testsvc: tools/testsvc.c
	$(CC) $(CFLAGS) -o $@ $<

cowdemo: tools/cowdemo.c
	$(CC) $(CFLAGS) -o $@ $<

%.o: %.c src/warden.h
	$(CC) $(CFLAGS) -c -o $@ $<

run: all
	./warden -c warden.conf

phase1:
	$(MAKE) -C phase1

check: all
	./tests/run_tests.sh

valgrind: all
	valgrind --leak-check=full --show-leak-kinds=all ./warden -c warden.conf

clean:
	rm -f $(OBJ) $(BINS)
	rm -rf logs
	$(MAKE) -C phase1 clean

.PHONY: all run phase1 check valgrind clean

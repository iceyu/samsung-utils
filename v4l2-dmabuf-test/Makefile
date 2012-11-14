CROSS_COMPILE ?= arm-linux-gnueabi-
KDIR ?= /usr/src/linux
CC=$(CROSS_COMPILE)gcc
OBJS = v4l2-dbuf
CFLAGS += -I$(KDIR)/usr/include -std=gnu99 -Wall -pedantic -O2

all:  $(OBJS)

%.o : %.c
	$(CC) $(CFLAGS) -c $< -o $@

% : %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f *.o
	rm -f $(OBJS)

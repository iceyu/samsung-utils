# Target root
#TARGETROOT?=
# Toolchain path
TCPATH?=arm-linux-gnueabi-
KERNELPATH?=

CC = ${TCPATH}gcc
AR = "${TCPATH}ar rc"
AR2 = ${TCPATH}ranlib make -j4

INCLUDES = -I$(KERNELPATH)/include -I$(KERNELPATH)/arch/arm/include/
SOURCES = fimc-m2m-f02.c
OBJECTS := $(SOURCES:.c=.o)
EXEC = fimc-m2m-f02
CFLAGS = -Wall -g

all: $(EXEC)

.c.o:
	$(CC) -c $(CFLAGS) $(INCLUDES) $<

$(EXEC): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(EXEC) $(OBJECTS) -lpthread #-L $(TARGETROOT)/usr/lib

clean:
	rm -f *.o $(EXEC)

install:


.PHONY: clean all

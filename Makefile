CFLAGS=-Wall -Wextra -pedantic -std=c99 -march=native -O3

all: afkcron 
afkcron: afkcron.c
	$(CC) ${CFLAGS} afkcron.c -lX11 -lXss -o afkcron

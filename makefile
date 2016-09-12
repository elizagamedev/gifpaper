#!/bin/make

CC=gcc
CFLAGS=-c -Wall -std=gnu99 -Os -ffast-math -DSINGLE
OUTPUT=gifpaper

all: build

build: gifpaper.o
	$(CC) gifpaper.o -o $(OUTPUT) -lX11 -lgif -lXrandr 

gifpaper.o:
	$(CC) gifpaper.c $(CFLAGS)

clean:
	rm -rf *.o

# gcc -o gifpaper gifpaper.c -Wall -pedantic -lX11 -lgif -lXrandr -std=gnu99 -Os -ffast-math -DSINGLE

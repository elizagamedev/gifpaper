#!/bin/bash
gcc -o gifpaper gifpaper.c -Wall -pedantic -lX11 -lgif -lXrandr -std=gnu99 -Os -ffast-math -DSINGLE

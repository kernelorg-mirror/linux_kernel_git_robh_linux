#!/bin/sh

cp include/uapi/drm/ethos_accel.h .
gcc -O2 -I/usr/include/drm -I/usr/include -o ethos-test ethos_test.c

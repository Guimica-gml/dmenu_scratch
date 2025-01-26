#!/usr/bin/sh
set -xe

CFLAGS="-Wall -Wextra -pedantic -ggdb -std=c99"
gcc $CFLAGS -o dmenu_scratch src/main.c src/json.c src/utils.c

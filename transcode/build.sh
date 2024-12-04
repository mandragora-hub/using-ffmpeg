#!/bin/bash

PKG_CONFIG_DEPENDECIES="libavcodec libavformat libavutil libswscale"
gcc -Wall -Wextra -g $(pkg-config --cflags "${PKG_CONFIG_DEPENDECIES}") main.c $(pkg-config --libs "${PKG_CONFIG_DEPENDECIES}") -o transcode.out


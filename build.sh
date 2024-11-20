#!/bin/bash
#

# PKG_CONFIG_DEPENDECIES="libavcodec libavformat"
# gcc -Wall -Wextra -g $(pkg-config --cflags "${PKG_CONFIG_DEPENDECIES}") show_codecs.c $(pkg-config --libs "${PKG_CONFIG_DEPENDECIES}") -o show_codecs 

# PKG_CONFIG_DEPENDECIES="libavcodec libavformat libavutil libswscale"
# gcc -Wall -Wextra -g $(pkg-config --cflags "${PKG_CONFIG_DEPENDECIES}") reading_video_frames.c $(pkg-config --libs "${PKG_CONFIG_DEPENDECIES}") -o reading_video_frames 

PKG_CONFIG_DEPENDECIES="libavcodec libavformat libavutil libswscale libavfilter"
gcc -Wall -Wextra -g $(pkg-config --cflags "${PKG_CONFIG_DEPENDECIES}") transcode.c $(pkg-config --libs "${PKG_CONFIG_DEPENDECIES}") -o transcode 

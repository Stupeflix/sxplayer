NAME = sxplayer

PROJECT_OBJS = log.o async.o decoders.o decoder_ffmpeg.o
DARWIN_OBJS  = decoder_vt.o
ANDROID_OBJS =

PROJECT_PKG_CONFIG_LIBS = libavformat libavfilter libavcodec libavutil
DARWIN_PKG_CONFIG_LIBS  =
ANDROID_PKG_CONFIG_LIBS =

PROJECT_LIBS = -lm -pthread
DARWIN_LIBS  = -framework CoreFoundation -framework VideoToolbox -framework CoreMedia -framework QuartzCore
ANDROID_LIBS =

include utils.inc

test: $(NAME)
	./$(NAME) media.mkv
testmem: $(NAME)
	valgrind --leak-check=full ./$(NAME) media.mkv

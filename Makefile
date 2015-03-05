NAME = sfxmp
FFMPEG_LIBS = libavformat libavfilter libavcodec libavutil

CFLAGS += -Wall -O2 -Werror=missing-prototypes -g
CFLAGS := $(shell pkg-config --cflags $(FFMPEG_LIBS)) $(CFLAGS)
LDLIBS := $(shell pkg-config --libs   $(FFMPEG_LIBS)) $(LDLIBS) -lpthread

OBJS = $(NAME).o main.o

all: $(NAME)
$(NAME): $(OBJS)
test: all
	./$(NAME) test.mkv
testmem: all
	valgrind --leak-check=full ./$(NAME) test.mkv
clean:
	$(RM) $(NAME) $(OBJS)

.PHONY: all clean

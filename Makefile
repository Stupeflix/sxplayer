PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config

NAME = sfxmp
LIBNAME = lib$(NAME).a
FFMPEG_LIBS = libavformat libavfilter libavcodec libavutil

CFLAGS += -Wall -O2 -Werror=missing-prototypes -g
CFLAGS := $(shell $(PKG_CONFIG) --cflags $(FFMPEG_LIBS)) $(CFLAGS)
LDLIBS := $(shell $(PKG_CONFIG) --libs   $(FFMPEG_LIBS)) $(LDLIBS) -lm -lpthread

TESTOBJS = main.o
OBJS = $(NAME).o

$(LIBNAME): $(OBJS)
	$(AR) rcs $@ $^

$(NAME): $(OBJS) $(TESTOBJS)

all: $(LIBNAME) $(NAME)

clean:
	$(RM) $(LIBNAME) $(NAME) $(OBJS) $(TESTOBJS)
test: $(NAME)
	./$(NAME) media.mkv
testmem: $(NAME)
	valgrind --leak-check=full ./$(NAME) media.mkv
install: $(LIBNAME)
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include
	install -m 644 $(LIBNAME) $(DESTDIR)$(PREFIX)/lib
	install -m 644 $(NAME).h $(DESTDIR)$(PREFIX)/include/$(NAME).h

uninstall:
	$(RM) $(DESTDIR)$(PREFIX)/lib/$(LIBNAME)
	$(RM) $(DESTDIR)$(PREFIX)/include/$(NAME).h

.PHONY: all clean

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config

NAME = sfxmp
LIBNAME = lib$(NAME).a
PCNAME  = lib$(NAME).pc
FFMPEG_LIBS = libavformat libavfilter libavcodec libavutil

CFLAGS += -Wall -O2 -Werror=missing-prototypes -g
CFLAGS := $(shell $(PKG_CONFIG) --cflags $(FFMPEG_LIBS)) $(CFLAGS)
LDLIBS := $(shell $(PKG_CONFIG) --libs   $(FFMPEG_LIBS)) $(LDLIBS) -lm -lpthread

TESTOBJS = main.o
OBJS = $(NAME).o

$(LIBNAME): $(OBJS)
	$(AR) rcs $@ $^

$(NAME): $(OBJS) $(TESTOBJS)

all: $(LIBNAME) $(PCNAME) $(NAME)

clean:
	$(RM) $(LIBNAME) $(NAME) $(OBJS) $(TESTOBJS) $(PCNAME)
test: $(NAME)
	./$(NAME) media.mkv
testmem: $(NAME)
	valgrind --leak-check=full ./$(NAME) media.mkv
$(PCNAME): $(PCNAME).tpl
	sed -e "s#PREFIX#$(PREFIX)#" -e "s#DEP_LIBS#$(LDLIBS)#" $^ > $@
install: $(LIBNAME) $(PCNAME)
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -d $(DESTDIR)$(PREFIX)/include
	install -m 644 $(LIBNAME) $(DESTDIR)$(PREFIX)/lib
	install -m 644 $(PCNAME) $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m 644 $(NAME).h $(DESTDIR)$(PREFIX)/include/$(NAME).h

uninstall:
	$(RM) $(DESTDIR)$(PREFIX)/lib/$(LIBNAME)
	$(RM) $(DESTDIR)$(PREFIX)/include/$(NAME).h

.PHONY: all clean

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config

SHARED ?= no

TARGET_OS ?= $(shell uname -s)
ifeq ($(SHARED),yes)
ifeq ($(TARGET_OS),Linux)
	LIBSUFFIX = so
else
ifeq ($(TARGET_OS),Darwin)
	LIBSUFFIX = dylib
endif # darwin
endif # linux
else
	LIBSUFFIX = a
endif # shared

NAME = sfxmp
LIBNAME = lib$(NAME).$(LIBSUFFIX)
PCNAME  = lib$(NAME).pc
FFMPEG_LIBS = libavformat libavfilter libavcodec libavutil

OBJS = $(NAME).o log.o

ifeq ($(TARGET_OS),Darwin)
EXTRALIBS = -framework CoreFoundation -framework VideoToolbox -framework CoreMedia -framework QuartzCore
OBJS += hwaccel_vt.o
endif

CFLAGS += -Wall -O2 -Werror=missing-prototypes -g -fPIC
CFLAGS := $(shell $(PKG_CONFIG) --cflags $(FFMPEG_LIBS)) $(CFLAGS)
LDLIBS := $(shell $(PKG_CONFIG) --libs   $(FFMPEG_LIBS)) $(LDLIBS) -lm -lpthread $(EXTRALIBS)

TESTOBJS = main.o

$(LIBNAME): $(OBJS)
ifeq ($(SHARED),yes)
	$(CC) $^ -shared -o $@ $(LDLIBS)
else
	$(AR) rcs $@ $^
endif

$(NAME): $(OBJS) $(TESTOBJS)

all: $(LIBNAME) $(PCNAME) $(NAME)

clean:
	$(RM) lib$(NAME).so lib$(NAME).dylib lib$(NAME).a $(NAME) $(OBJS) $(TESTOBJS) $(PCNAME)
test: $(NAME)
	./$(NAME) media.mkv
testmem: $(NAME)
	valgrind --leak-check=full ./$(NAME) media.mkv
$(PCNAME): $(PCNAME).tpl
ifeq ($(SHARED),yes)
	sed -e "s#PREFIX#$(PREFIX)#;s#DEP_LIBS##;s#DEP_PRIVATE_LIBS#$(LDLIBS)#" $^ > $@
else
	sed -e "s#PREFIX#$(PREFIX)#;s#DEP_LIBS#$(LDLIBS)#;s#DEP_PRIVATE_LIBS##" $^ > $@
endif
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

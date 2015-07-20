PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config

SHARED ?= no
DEBUG  ?= no

TARGET_OS ?= $(shell uname -s)

DYLIBSUFFIX = so
ifeq ($(TARGET_OS),Darwin)
	DYLIBSUFFIX = dylib
	PROJECT_LIBS            += $(DARWIN_LIBS)
	PROJECT_PKG_CONFIG_LIBS += $(DARWIN_PKG_CONFIG_LIBS)
	PROJECT_OBJS            += $(DARWIN_OBJS)
else
ifeq ($(TARGET_OS),Android)
	PROJECT_LIBS            += $(ANDROID_LIBS)
	PROJECT_PKG_CONFIG_LIBS += $(ANDROID_PKG_CONFIG_LIBS)
	PROJECT_OBJS            += $(ANDROID_OBJS)
endif # android
endif # darwin

ifeq ($(SHARED),yes)
	LIBSUFFIX = $(DYLIBSUFFIX)
else
	LIBSUFFIX = a
endif

LIBNAME = lib$(NAME).$(LIBSUFFIX)
PCNAME  = lib$(NAME).pc

OBJS += $(NAME).o $(PROJECT_OBJS)

CFLAGS += -Wall -O2 -Werror=missing-prototypes -fPIC
ifeq ($(DEBUG),yes)
	CFLAGS += -g
endif
CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PROJECT_PKG_CONFIG_LIBS)) $(CFLAGS)
LDLIBS := $(shell $(PKG_CONFIG) --libs   $(PROJECT_PKG_CONFIG_LIBS)) $(LDLIBS) $(PROJECT_LIBS)

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
	$(RM) lib$(NAME).so lib$(NAME).dylib lib$(NAME).a $(NAME).hpp $(NAME) $(OBJS) $(TESTOBJS) $(PCNAME)
$(PCNAME): $(PCNAME).tpl
ifeq ($(SHARED),yes)
	sed -e "s#PREFIX#$(PREFIX)#;s#DEP_LIBS##;s#DEP_PRIVATE_LIBS#$(LDLIBS)#" $^ > $@
else
	sed -e "s#PREFIX#$(PREFIX)#;s#DEP_LIBS#$(LDLIBS)#;s#DEP_PRIVATE_LIBS##" $^ > $@
endif

# because fuck sanity
$(NAME).hpp: $(NAME).h
	@printf "#warning \"$(NAME) is a C library and C++ is not officially supported\"\nextern \"C\" {\n#include \"$(NAME).h\"\n}\n" > $(NAME).hpp

install: $(LIBNAME) $(PCNAME) $(NAME).hpp
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -d $(DESTDIR)$(PREFIX)/include
	install -m 644 $(LIBNAME) $(DESTDIR)$(PREFIX)/lib
	install -m 644 $(PCNAME) $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m 644 $(NAME).h $(DESTDIR)$(PREFIX)/include/$(NAME).h
	install -m 644 $(NAME).hpp $(DESTDIR)$(PREFIX)/include/$(NAME).hpp

uninstall:
	$(RM) $(DESTDIR)$(PREFIX)/lib/$(LIBNAME)
	$(RM) $(DESTDIR)$(PREFIX)/include/$(NAME).h
	$(RM) $(DESTDIR)$(PREFIX)/include/$(NAME).hpp

.PHONY: all clean install uninstall

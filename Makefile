# fmedia v0.14 makefile

PROJ := fmedia
ROOT := ..
PROJDIR := $(ROOT)/fmedia
SRCDIR := $(PROJDIR)/src
VER :=
OS :=
OPT := LTO3

FFOS := $(ROOT)/ffos
FF := $(ROOT)/ff
FF3PT := $(ROOT)/ff-3pt

include $(FFOS)/makeconf


# OS-specific options
ifeq ($(OS),win)
BIN := fmedia.exe
INSTDIR := fmedia
CFLAGS_OS += -DFF_WIN=0x0501

else
BIN := fmedia
INSTDIR := fmedia-1
ifeq ($(OS),linux)
CFLAGS_OS += -DFF_GLIBCVER=228
endif
endif


FF_OBJ_DIR := ./ff-obj
FFOS_CFLAGS := $(CFLAGS_STD) $(CFLAGS_DEBUG) $(CFLAGS_OPT) $(CFLAGS_OS) $(CFLAGS_CPU) -pthread
FF_CFLAGS := $(CFLAGS_STD) $(CFLAGS_DEBUG) $(CFLAGS_OPT) $(CFLAGS_OS) $(CFLAGS_CPU)
FF3PTLIB := $(FF3PT)-bin/$(OS)-$(ARCH)
FF3PT_CFLAGS := $(CFLAGS_STD) $(CFLAGS_DEBUG) $(CFLAGS_OPT) $(CFLAGS_OS) $(CFLAGS_CPU)


# CPU-specific options
ifeq ($(CPU),i686)
CFLAGS_CPU += -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
endif


CFLAGS_APP := \
	-DFFS_FMT_NO_e \
	-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-implicit-fallthrough \
	-I$(SRCDIR) -I$(FF) -I$(FFOS) -I$(FF3PT)
CFLAGS := $(CFLAGS_STD) $(CFLAGS_DEBUG) $(CFLAGS_OPT) $(CFLAGS_OS) $(CFLAGS_CPU) $(CFLAGS_APP)
# alternative optimization flags: no LTO
ifneq ($(OPT),0)
	CFLAGS_OPT := -O3
endif
CFLAGS_ALTOPT := $(CFLAGS_STD) $(CFLAGS_DEBUG) $(CFLAGS_OPT) $(CFLAGS_OS) $(CFLAGS_CPU) $(CFLAGS_APP)
LDFLAGS += -L$(FF3PTLIB)

include $(PROJDIR)/makerules

package:
	rm -f $(PROJ)-$(VER)-$(OS)-$(ARCH_OS).$(PACK_EXT)
	$(PACK) $(PROJ)-$(VER)-$(OS)-$(ARCH_OS).$(PACK_EXT) $(INSTDIR)
	$(PACK) $(PROJ)-$(VER)-$(OS)-$(ARCH_OS)-debug.$(PACK_EXT) ./*.debug

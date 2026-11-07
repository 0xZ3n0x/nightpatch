# Top-level build for the Nightreign application project.
#
# The trapkit engine is a git submodule: this Makefile builds it first, then
# compiles nightpatch.ko against the engine's symbol table (ModVERSIONS CRCs
# shared via KBUILD_EXTRA_SYMBOLS), so mismatched engine/application builds
# are caught at load time. Header-only users (nightctl) compile from here.
# All artifacts go into ./build/ (and third_party/trapkit/build/ inside the submodule).

KDIR ?= /lib/modules/$(shell uname -r)/build
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

BUILDDIR := build
KMODDIR := $(BUILDDIR)/kmod
TKDIR := third_party/trapkit
TKMODULE := $(TKDIR)/build/kmod/trapkit.ko
TKSYMVERS := $(CURDIR)/$(TKDIR)/build/kmod/Module.symvers
NRSRCDIR := src/nightpatch

NR_HDRS := np_ioctl.h nr_internal.h
NR_SRC := nr_main.c nr_chardev.c nr_sites.c nr_softfloat.c
NR_OBJS := $(NR_SRC:.c=.o)

.PHONY: all kmod user clean load unload reload status

all: kmod user

# The engine module is built inside the submodule; only the application
# sources are staged locally here.
kmod: trapkit-module | $(KMODDIR)
	cp -u $(addprefix $(NRSRCDIR)/,$(NR_HDRS) $(NR_SRC)) $(KMODDIR)/
	# The application sees the engine through the public header only; the
	# engine's sources live inside the submodule.
	cp -u $(TKDIR)/src/tk.h $(KMODDIR)/
	$(MAKE) -C $(KDIR) M=$(CURDIR)/$(KMODDIR) KBUILD_EXTRA_SYMBOLS=$(TKSYMVERS) modules

trapkit-module:
	$(MAKE) -C $(TKDIR) module

$(KMODDIR):
	mkdir -p $(KMODDIR)
	printf 'obj-m += nightpatch.o\nnightpatch-objs := $(NR_OBJS)\n' > $(KMODDIR)/Makefile

user: $(BUILDDIR)/nightctl

$(BUILDDIR)/nightctl: src/nightctl.c $(NRSRCDIR)/np_ioctl.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ src/nightctl.c

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)
	$(MAKE) -C $(TKDIR) clean

load: kmod
	sudo insmod $(TKMODULE)
	sudo insmod $(KMODDIR)/nightpatch.ko

unload:
	-sudo rmmod nightpatch
	-sudo rmmod trapkit

reload: unload load

status: user
	sudo $(BUILDDIR)/nightctl status

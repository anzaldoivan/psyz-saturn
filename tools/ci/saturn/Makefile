ifeq ($(strip $(YAUL_INSTALL_ROOT)),)
  $(error Undefined YAUL_INSTALL_ROOT (install root directory))
endif

SH_PROGRAM:= psyz-saturn-title
include $(YAUL_INSTALL_ROOT)/share/build.pre.mk

PSYZ_ROOT ?= ../../..
include $(PSYZ_ROOT)/psyz/src/saturn/saturn.mk

SH_SRCS:= title.c $(PSYZ_SATURN_SRCS)
SH_CFLAGS+= -Os $(PSYZ_SATURN_CFLAGS)
SH_DEFSYMS:= \
	-Wl,--defsym=___master_stack=0x06100000 \
	-Wl,--defsym=___slave_stack=0x060FF000

include $(YAUL_INSTALL_ROOT)/share/build.post.bin.mk

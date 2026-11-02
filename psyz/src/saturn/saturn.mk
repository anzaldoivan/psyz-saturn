# Saturn build for the PsyZ flat scene, built from libyaul build.pre.mk.
# Usage: set SH_SRCS from PSYZ_SATURN_SRCS and SH_CFLAGS from PSYZ_SATURN_CFLAGS
# in the title Makefile (paths relative to the repo root: PSYZ_ROOT).
# Same role as psx.mk for the PS1 build.
PSYZ_ROOT ?= ../..

PSYZ_SATURN_SRCS := \
    $(PSYZ_ROOT)/decomp/src/libgpu/ext.c \
    $(PSYZ_ROOT)/decomp/src/libgpu/prim.c \
    $(PSYZ_ROOT)/decomp/src/libgpu/sys.c \
    $(PSYZ_ROOT)/psyz/src/psyz/kernel.c \
    $(PSYZ_ROOT)/psyz/src/psyz/libapi.c \
    $(PSYZ_ROOT)/psyz/src/psyz/libetc.c \
    $(PSYZ_ROOT)/psyz/src/psyz/libgpu.c \
    $(PSYZ_ROOT)/psyz/src/saturn/saturn_gpu.c \
    $(PSYZ_ROOT)/psyz/src/saturn/saturn_misc.c \
    $(PSYZ_ROOT)/psyz/src/saturn/saturn_os.c \
    $(PSYZ_ROOT)/psyz/src/saturn/saturn_pad.c

# core and decomp code type-pun (RECT shorts read as u32)
PSYZ_SATURN_CFLAGS := -D__psyz \
    -fno-strict-aliasing \
    -I$(PSYZ_ROOT)/psyz/include \
    -I$(PSYZ_ROOT)/psyz/src \
    -I$(PSYZ_ROOT)/psyz/src/psyz

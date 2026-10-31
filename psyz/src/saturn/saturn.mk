# Saturn build for the PsyZ flat scene, built from libyaul build.pre.mk.
# Usage: set SH_SRCS from PSYZ_SATURN_SRCS and SH_CFLAGS from PSYZ_SATURN_CFLAGS
# in the title Makefile (paths relative to the repo root: PSYZ_ROOT).
# Same role as psx.mk for the PS1 build; psyz/CMakeLists.txt carries the same list.
# A title that ships its own C libcd/libds/libsnd/libspu filter-outs those from its own
# source list before appending $(PSYZ_SATURN_SRCS); never filter PSYZ_SATURN_SRCS itself
# (%libcd.c would also drop saturn_libcd.c).
PSYZ_ROOT ?= ../..

PSYZ_SATURN_SRCS := \
    $(PSYZ_ROOT)/decomp/src/libgpu/ext.c \
    $(PSYZ_ROOT)/decomp/src/libgpu/prim.c \
    $(PSYZ_ROOT)/decomp/src/libgpu/sys.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/cres.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/decres.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/pause.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/play.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/playmode.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/replay.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/sscall.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ssclose.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/sscres.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ssend.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ssinit.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ssinit_c.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ssinit_h.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ssloop.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ssopenp.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ssplay.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ssquit.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/sssattr.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/sssmv.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ssstart.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ssstop.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/sssv.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/sstable.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/sstick.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/sstickcb.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ssvol.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ut_ako.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ut_autop.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ut_cadsr.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ut_key.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ut_keyv.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ut_rdel.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ut_rdep.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ut_rev.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ut_roff.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ut_ron.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/ut_vvol.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vm_aloc2.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vm_autop.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vm_doff.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vm_f.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vm_g.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vm_init.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vm_n2p.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vm_noff.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vm_nowof.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vm_nowon.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vm_vsu.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vol.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vs_mono.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vs_srv.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vs_vab.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vs_vh.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vs_vtb.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vs_vtbp.c \
    $(PSYZ_ROOT)/decomp/src/libsnd/vs_vtc.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_cb.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_crwa.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_dcb.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_gtm.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_gva.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_i.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_ih.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_ini.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_it.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_itc.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_m.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_m_f.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_m_init.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_m_int.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_m_m.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_m_util.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_m_wsa.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_q.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_r.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_rmp.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_sca.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_sic.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_sk.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_snv.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_sr.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_stm.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_stsa.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_w.c \
    $(PSYZ_ROOT)/decomp/src/libspu/s_wp.c \
    $(PSYZ_ROOT)/decomp/src/libspu/spu.c \
    $(PSYZ_ROOT)/decomp/src/libspu/sr_gaks.c \
    $(PSYZ_ROOT)/psyz/src/psyz/libapi.c \
    $(PSYZ_ROOT)/psyz/src/psyz/libetc.c \
    $(PSYZ_ROOT)/psyz/src/psyz/libgpu.c \
    $(PSYZ_ROOT)/psyz/src/psyz/libgte.c \
    $(PSYZ_ROOT)/psyz/src/psyz/libgte_sw_sqrt.c \
    $(PSYZ_ROOT)/psyz/src/psyz/libsn.c \
    $(PSYZ_ROOT)/psyz/src/psyz/libsnd.c \
    $(PSYZ_ROOT)/psyz/src/psyz/libspu.c \
    $(PSYZ_ROOT)/psyz/src/saturn/saturn_cd.c \
    $(PSYZ_ROOT)/psyz/src/saturn/saturn_cdhw.c \
    $(PSYZ_ROOT)/psyz/src/saturn/saturn_gpu.c \
    $(PSYZ_ROOT)/psyz/src/saturn/saturn_libcd.c \
    $(PSYZ_ROOT)/psyz/src/saturn/saturn_misc.c \
    $(PSYZ_ROOT)/psyz/src/saturn/saturn_pad.c \
    $(PSYZ_ROOT)/psyz/src/saturn/saturn_save.c \
    $(PSYZ_ROOT)/psyz/src/saturn/saturn_spu.c

# The movie service and its decoder: a movie title appends these to its own
# sources; never in PSYZ_SATURN_SRCS (it would link into every title).
PSYZ_SATURN_MOVIE_SRCS := \
    $(PSYZ_ROOT)/psyz/src/saturn/saturn_movie.c \
    $(PSYZ_ROOT)/psyz/src/saturn/saturn_movie_decoder.c

# -include: getenv/strtoull prototypes libyaul's stdlib.h lacks (saturn_libc.h).
# -fno-strict-aliasing: core and decomp code type-pun (libgpu.c:433-434 reads
# RECT shorts as u32 right after storing them; sys.c likewise), which -Os
# strict aliasing may reorder; makes the puns behave as written.
PSYZ_SATURN_CFLAGS := -D__psyz \
    -fno-strict-aliasing \
    -include $(PSYZ_ROOT)/psyz/src/saturn/saturn_libc.h \
    -I$(PSYZ_ROOT)/psyz/include \
    -I$(PSYZ_ROOT)/psyz/src \
    -I$(PSYZ_ROOT)/psyz/src/psyz

TOP = ../../../..
include $(TOP)/configs/current

LIBBASENAME = vdpau_r600

PIPE_DRIVERS = \
        $(TOP)/src/gallium/drivers/r600/libr600.a \
	$(TOP)/src/gallium/winsys/g3dvl/dri/libvldri.a \
	$(TOP)/src/gallium/winsys/radeon/drm/libradeonwinsys.a \
        $(TOP)/src/gallium/drivers/trace/libtrace.a \
	$(TOP)/src/gallium/auxiliary/libgallium.a

C_SOURCES = \
	target.c \
	$(COMMON_GALLIUM_SOURCES) \
	$(DRIVER_SOURCES)

DRIVER_LIBS = $(shell $(PKG_CONFIG) libdrm --libs) -lXfixes -ldrm_radeon

include ../Makefile.vdpau

symlinks:

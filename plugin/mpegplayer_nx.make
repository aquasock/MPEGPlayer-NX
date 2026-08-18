# SPDX-License-Identifier: GPL-2.0-or-later

MPEGPLAYER_NX_SRCDIR := $(APPSDIR)/plugins/mpegplayer_nx
MPEGPLAYER_NX_BUILDDIR := $(BUILDDIR)/apps/plugins/mpegplayer_nx

ROCKS += $(MPEGPLAYER_NX_BUILDDIR)/mpegplayer_nx.rock

MPEGPLAYER_NX_SRC := $(call preprocess, $(MPEGPLAYER_NX_SRCDIR)/SOURCES)
MPEGPLAYER_NX_OBJ := $(call c2obj, $(MPEGPLAYER_NX_SRC))

# Add source files to OTHER_SRC to get automatic dependencies.
OTHER_SRC += $(MPEGPLAYER_NX_SRC)

$(MPEGPLAYER_NX_BUILDDIR)/mpegplayer_nx.rock: $(MPEGPLAYER_NX_OBJ) $(TLSFLIB) \
                                            $(FAADLIB) $(CODECLIB)

$(MPEGPLAYER_NX_BUILDDIR)/%.o: $(MPEGPLAYER_NX_SRCDIR)/%.c \
                                      $(MPEGPLAYER_NX_SRCDIR)/mpegplayer_nx.make
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) -I$(MPEGPLAYER_NX_SRCDIR) \
		-I$(MPEGPLAYER_NX_SRCDIR)/h264bsd -I$(TLSFLIB_DIR)/src \
		-I$(RBCODECLIB_DIR)/codecs -I$(RBCODECLIB_DIR)/codecs/lib \
		-I$(RBCODECLIB_DIR)/codecs/libfaad \
		-DMPEGPLAYER_NX_DECODER $(PLUGINFLAGS) -c $< -o $@

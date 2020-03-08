NULL =
SUBDIRS = .

if ENABLE_TESTS
SUBDIRS += tests

check-valgrind:
	$(MAKE) -C tests check-valgrind
endif

AM_CPPFLAGS =					\
	-DSPICE_SERVER_INTERNAL			\
	$(SPICE_COMMON_CFLAGS)			\
	$(GLIB2_CFLAGS)				\
	$(LZ4_CFLAGS)				\
	$(PIXMAN_CFLAGS)			\
	$(SASL_CFLAGS)				\
	$(SMARTCARD_CFLAGS)			\
	$(GSTREAMER_0_10_CFLAGS)		\
	$(GSTREAMER_1_0_CFLAGS)			\
	$(SSL_CFLAGS)				\
	$(VISIBILITY_HIDDEN_CFLAGS)		\
	$(ORC_CFLAGS)				\
	$(NULL)

AM_CFLAGS =					\
	$(WARN_CFLAGS)				\
	$(NULL)

AM_CXXFLAGS =					\
	$(WARN_CXXFLAGS)			\
	$(NULL)

noinst_LTLIBRARIES = libserver.la
lib_LTLIBRARIES = libspice-server.la

libspice_server_la_LDFLAGS =			\
	-version-info $(SPICE_LT_VERSION)	\
	-no-undefined				\
	$(RELRO_LDFLAGS)			\
	$(NO_INDIRECT_LDFLAGS)			\
	$(NULL)

if HAVE_LD_VERSION_SCRIPT
libspice_server_la_LDFLAGS += \
	-Wl,--version-script=$(srcdir)/spice-server.syms
EXTRA_libspice_server_la_DEPENDENCIES = $(srcdir)/spice-server.syms
endif

libserver_la_LIBADD =							\
	$(SPICE_COMMON_DIR)/common/libspice-common.la			\
	$(SPICE_COMMON_DIR)/common/libspice-common-server.la		\
	$(GLIB2_LIBS)							\
	$(JPEG_LIBS)							\
	$(LZ4_LIBS)							\
	$(LIBRT)							\
	$(PIXMAN_LIBS)							\
	$(SASL_LIBS)							\
	$(GSTREAMER_0_10_LIBS)						\
	$(GSTREAMER_1_0_LIBS)						\
	$(SSL_LIBS)							\
	$(Z_LIBS)							\
	$(SPICE_NONPKGCONFIG_LIBS)					\
	$(ORC_LIBS)							\
	$(NULL)

libspice_serverincludedir = $(includedir)/spice-server
libspice_serverinclude_HEADERS =		\
	spice-audio.h				\
	spice-char.h				\
	spice-core.h				\
	spice-input.h				\
	spice-migration.h			\
	spice-qxl.h				\
	spice-server.h				\
	spice-version.h				\
	spice-replay.h				\
	spice.h					\
	$(NULL)

spice_built_sources = spice-server-enums.h spice-server-enums.c

libserver_la_SOURCES =				\
	$(libspice_serverinclude_HEADERS)	\
	$(spice_built_sources)			\
	agent-msg-filter.c			\
	agent-msg-filter.h			\
	cache-item.h				\
	char-device.cpp				\
	char-device.h				\
	common-graphics-channel.cpp		\
	common-graphics-channel.h		\
	cursor-channel.cpp			\
	cursor-channel-client.cpp		\
	cursor-channel-client.h			\
	cursor-channel.h			\
	utils.hpp				\
	safe-list.hpp				\
	dcc.cpp					\
	dcc.h					\
	dcc-private.h				\
	dcc-send.cpp				\
	dispatcher.cpp				\
	dispatcher.h				\
	display-channel.cpp			\
	display-channel.h			\
	display-channel-private.h		\
	display-limits.h			\
	event-loop.c				\
	glz-encoder.c				\
	glz-encoder-dict.c			\
	glz-encoder-dict.h			\
	glz-encoder.h				\
	glz-encoder-priv.h			\
	image-cache.cpp				\
	image-cache.h				\
	image-encoders.cpp			\
	image-encoders.h			\
	inputs-channel.cpp			\
	inputs-channel-client.cpp		\
	inputs-channel-client.h			\
	inputs-channel.h			\
	jpeg-encoder.c				\
	jpeg-encoder.h				\
	main-channel.cpp			\
	main-channel-client.cpp			\
	main-channel-client.h			\
	main-channel.h				\
	main-dispatcher.cpp			\
	main-dispatcher.h			\
	memslot.c				\
	memslot.h				\
	migration-protocol.h			\
	mjpeg-encoder.c				\
	net-utils.c				\
	net-utils.h				\
	pixmap-cache.cpp			\
	pixmap-cache.h				\
	pop-visibility.h			\
	push-visibility.h			\
	red-channel.cpp				\
	red-channel-capabilities.c		\
	red-channel-capabilities.h		\
	red-channel-client.cpp			\
	red-channel-client.h			\
	red-channel.h				\
	red-client.cpp				\
	red-client.h				\
	red-common.h				\
	red-parse-qxl.cpp			\
	red-parse-qxl.h				\
	red-pipe-item.cpp			\
	red-pipe-item.h				\
	red-qxl.cpp				\
	red-qxl.h				\
	red-record-qxl.c			\
	red-record-qxl.h			\
	red-replay-qxl.cpp			\
	reds.cpp				\
	reds.h					\
	reds-private.h				\
	red-stream.cpp				\
	red-stream.h				\
	red-worker.cpp				\
	red-worker.h				\
	sound.cpp				\
	sound.h					\
	spice-bitmap-utils.c			\
	spice-bitmap-utils.h			\
	spicevmc.cpp				\
	spice-wrapped.h				\
	stat-file.c				\
	stat-file.h				\
	stat.h					\
	stream-channel.cpp			\
	stream-channel.h			\
	sys-socket.h				\
	sys-socket.c				\
	red-stream-device.cpp			\
	red-stream-device.h			\
	sw-canvas.c				\
	tree.cpp				\
	tree.h					\
	utils.c					\
	utils.h					\
	video-encoder.h				\
	video-stream.cpp			\
	video-stream.h				\
	websocket.c				\
	websocket.h				\
	zlib-encoder.c				\
	zlib-encoder.h				\
	$(NULL)

if HAVE_LZ4
libserver_la_SOURCES +=				\
	lz4-encoder.c				\
	lz4-encoder.h				\
	$(NULL)
endif

if HAVE_SMARTCARD
libserver_la_SOURCES +=			\
	smartcard.cpp			\
	smartcard.h			\
	smartcard-channel-client.cpp	\
	smartcard-channel-client.h	\
	$(NULL)
endif

if HAVE_GSTREAMER
libserver_la_SOURCES +=			\
	gstreamer-encoder.c		\
	$(NULL)
endif

libspice_server_la_LIBADD = libserver.la
libspice_server_la_SOURCES =
## see https://www.gnu.org/software/automake/manual/html_node/Libtool-Convenience-Libraries.html
## ("Libtool Convenience Libraries")
nodist_EXTRA_libspice_server_la_SOURCES = dummy.cpp

spice-server-enums.c: spice-server.h spice-server-enums.c.tmpl
	$(AM_V_GEN)glib-mkenums --template $(srcdir)/spice-server-enums.c.tmpl $< > $@

spice-server-enums.h: spice-server.h spice-server-enums.h.tmpl
	$(AM_V_GEN)glib-mkenums --template $(srcdir)/spice-server-enums.h.tmpl $< > $@

EXTRA_DIST =					\
	meson.build				\
	spice-bitmap-utils.tmpl.c		\
	cache-item.tmpl.cpp			\
	glz-encode-match.tmpl.c			\
	glz-encode.tmpl.c			\
	spice-server.syms			\
	spice-server-enums.h.tmpl		\
	spice-server-enums.c.tmpl		\
	$(NULL)

BUILT_SOURCES = $(spice_built_sources)

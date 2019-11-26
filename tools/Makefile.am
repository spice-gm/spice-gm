NULL =

AM_CPPFLAGS = \
	$(SPICE_COMMON_CFLAGS) \
	$(WARN_CFLAGS) \
	$(NULL)

AM_LDFLAGS = \
	$(LIBRT) \
	$(NULL)

if !OS_WIN32
noinst_PROGRAMS = \
	reds_stat \
	$(NULL)

reds_stat_SOURCES = \
	reds_stat.c \
	$(NULL)
endif

EXTRA_DIST = \
	meson.build \
	$(NULL)

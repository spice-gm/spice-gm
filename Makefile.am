NULL =
ACLOCAL_AMFLAGS = -I m4

SUBDIRS = subprojects/spice-common server docs tools

check-valgrind:
	$(MAKE) -C server check-valgrind

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = spice-server.pc

DISTCHECK_CONFIGURE_FLAGS =			\
	--disable-celt051			\
	--enable-smartcard			\
	--with-sasl				\
	--enable-manual				\
	$(NULL)

EXTRA_DIST =					\
	meson.build				\
	meson_options.txt			\
	build-aux/git-version-gen		\
	build-aux/meson/check-spice-common	\
	CHANGELOG.md				\
	.version				\
	$(NULL)

# see git-version-gen
dist-hook:
	echo $(VERSION) > $(distdir)/.tarball-version

BUILT_SOURCES = $(top_srcdir)/.version
$(top_srcdir)/.version:
	echo $(VERSION) > $@-t && mv $@-t $@

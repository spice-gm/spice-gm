NULL =
ASCIIDOC_FLAGS = -a icons -a toc

EXTRA_DIST =					\
	meson.build				\
	spice_style.html			\
	spice_style.txt				\
	spice_threading_model.html		\
	spice_threading_model.txt		\
	$(NULL)

if BUILD_MANUAL
SUBDIRS = manual

all-local: spice_style.html spice_threading_model.html

%.html: %.txt
	$(AM_V_GEN) $(ASCIIDOC) -n $(ASCIIDOC_FLAGS) -o $@ $<
endif

clean-local:
	rm -f spice_style.html spice_threading_model.html

# -*- Makefile -*-

AM_CPPFLAGS = ${regular_CPPFLAGS} ${babl_CFLAGS} ${eigen_CFLAGS} ${libHX_CFLAGS}
AM_CXXFLAGS = ${regular_CXXFLAGS}
am__tar = tar --owner=0 --group=0 --numeric-owner --format=posix -chf - "$$tardir"

bin_PROGRAMS = palcomp vfontas
dist_bin_SCRIPTS = cp437table unicode_table
EXTRA_DIST = doc/changelog.rst doc/vfontas-formats.dot src/glynames.cpp LICENSE.GPL3 LICENSE.MIT
dist_pkgdata_DATA = cp437x.uni cp1090f.uni

palcomp_SOURCES = src/palcomp.cpp
palcomp_LDADD = -lm ${babl_LIBS} ${libHX_LIBS} ${eigen_LIBS}
vfontas_SOURCES = src/vfontas.cpp src/vfalib.cpp src/vfalib.hpp
vfontas_LDADD = ${libHX_LIBS}
dist_man1_MANS = doc/palcomp.1 doc/vfontas.1

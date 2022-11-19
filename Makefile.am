# -*- Makefile -*-

AM_CPPFLAGS = ${regular_CPPFLAGS} ${libHX_CFLAGS}
AM_CFLAGS   = ${regular_CFLAGS}
AM_CXXFLAGS = ${regular_CXXFLAGS}

bin_PROGRAMS = palcomp vfontas

palcomp_SOURCES = src/palcomp.cpp
palcomp_LDADD = -lm
vfontas_SOURCES = src/vfontas.cpp src/vfalib.cpp src/vfalib.hpp
vfontas_LDADD = ${libHX_LIBS}
man1_MANS = doc/palcomp.1 doc/vfontas.1

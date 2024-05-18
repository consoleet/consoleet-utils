AC_INIT([consoleet-utils], [1.5])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_HEADERS([config.h])
AC_PROG_INSTALL
AM_INIT_AUTOMAKE([-Wall foreign subdir-objects tar-pax no-dist-gzip dist-zstd])
m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])
AC_PROG_CXX

regular_CPPFLAGS="-D_FILE_OFFSET_BITS=64 -D_REENTRANT"
regular_CFLAGS="-Wall -Waggregate-return -Wmissing-declarations \
	-Wmissing-prototypes -Wredundant-decls -Wshadow -Wstrict-prototypes \
	-Winline -pipe -std=gnu11"
regular_CXXFLAGS="-Wall -Wno-pointer-arith -Wshadow -pipe -std=gnu++17"
AC_SUBST([regular_CPPFLAGS])
AC_SUBST([regular_CFLAGS])
AC_SUBST([regular_CXXFLAGS])

PKG_CHECK_MODULES([babl], [babl-0.1])
PKG_CHECK_MODULES([libHX], [libHX >= 4.15])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

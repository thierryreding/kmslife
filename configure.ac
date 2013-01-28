AC_PREREQ(2.58)

AC_INIT([kmslife], [0.0.0], [thierry@gilfi.de])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([Makefile.am])
AC_CONFIG_HEADER([config.h])
AC_CANONICAL_HOST

AM_INIT_AUTOMAKE([dist-xz no-dist-gzip foreign])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_INSTALL

PKG_CHECK_MODULES(DRM, libdrm)

CFLAGS="$CFLAGS -Wall -Werror"
AC_SUBST(CFLAGS)

AC_OUTPUT([
	Makefile
])

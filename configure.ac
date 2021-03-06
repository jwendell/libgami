AC_INIT([libgami],[0.4])
AC_CANONICAL_SYSTEM
AM_INIT_AUTOMAKE(AC_PACKAGE_NAME, AC_PACKAGE_VERSION)

m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

AC_CONFIG_HEADERS([config.h])

AC_ISC_POSIX
AC_PROG_CC
AM_PROG_CC_STDC
AC_HEADER_STDC
IT_PROG_INTLTOOL([0.35.0])


##################################################
# Portability
##################################################

AC_MSG_CHECKING([for Win32])
case "$host" in
  *-*-mingw*)
    AC_MSG_RESULT([yes])

    AC_DEFINE([WINVER],[0x0501],[Set required API version when on windows])
    LDFLAGS="$LDFLAGS -lws2_32 -no-undefined"

    AC_MSG_CHECKING([for gai_strerror])
    AC_LINK_IFELSE(
        [AC_LANG_PROGRAM([[
                #include <windef.h>
                #include <ws2tcpip.h>
            ]],[[
                gai_strerror (0);
            ]])
        ],
        [have_gai_strerror=yes],
        [have_gai_strerror=no])
    AC_MSG_RESULT([$have_gai_strerror])
  ;;
  *)
    AC_MSG_RESULT([no])

    AC_CHECK_FUNC([gai_strerror],
	[have_gai_strerror=yes],
	[have_gai_strerror=no])
  ;;
esac

if test "$have_gai_strerror" = "yes";
then
	AC_DEFINE([HAVE_GAI_STRERROR],[1],
		[Define if a usable gai_strerror exists])
fi


##################################################
# Internationalization
##################################################

GETTEXT_PACKAGE=libgami
AC_SUBST(GETTEXT_PACKAGE)
AC_DEFINE_UNQUOTED([GETTEXT_PACKAGE],["$GETTEXT_PACKAGE"],[Gettext package])
AM_GLIB_GNU_GETTEXT
AM_GLIB_DEFINE_LOCALEDIR(GAMI_LOCALEDIR)
 

##################################################
# Libtool
##################################################
m4_defun([_LT_AC_LANG_CXX_CONFIG], [:]) dnl disable checks for C++
m4_defun([_LT_AC_LANG_F77_CONFIG], [:]) dnl disable checks for Fortran
m4_defun([_LT_AC_LANG_GCJ_CONFIG], [:]) dnl disable checks for (GNU) Java
AC_LIBTOOL_WIN32_DLL
AC_PROG_LIBTOOL


##################################################
# Module dependency
##################################################

GLIB_REQ=2.16
PKG_CHECK_MODULES([GAMI], [glib-2.0 >= $GLIB_REQ gobject-2.0 gio-2.0])


##################################################
# GObject Introspection
##################################################

GOBJECT_INTROSPECTION_CHECK([0.6.3])

##################################################
# Check for gtk-doc.
##################################################

GTK_DOC_CHECK([1.9])


AC_OUTPUT([
Makefile
libgami-1.0.pc
src/Makefile
po/Makefile.in
docs/Makefile
docs/reference/Makefile
docs/reference/version.xml
])

echo "
Configure summary:
      Gtk-Doc Support........:  $enable_gtk_doc
      GObj. Introspection....:  $enable_introspection

Now type 'make' to build.
"

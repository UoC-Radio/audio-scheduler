#Prelude
AC_INIT([audio-scheduler],[0.5],[radio-list@culture.uoc.gr])
AC_CONFIG_SRCDIR([main.c])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([foreign -Wall -Werror dist-bzip2])

#Configuration / define macros
AC_ARG_WITH([debug],
	    AS_HELP_STRING([--with-debug],
			   [Enable debug output]),
	    AC_DEFINE([DEBUG],[1],
		      [Define this to enable debug output]),
	    [])

# Check for programs
AC_PROG_CC

# Check for libraries
PKG_CHECK_MODULES(AVFORMAT, [ libavformat >= 5.0 ],
		[
			AC_SUBST(AVFORMAT_CFLAGS)
			AC_SUBST(AVFORMAT_LIBS)
		],
		[ AC_MSG_ERROR([libavformat not found]) ])
PKG_CHECK_MODULES(AVCODEC, [ libavcodec >= 5.0],
		[
			AC_SUBST(AVCODEC_CFLAGS)
			AC_SUBST(AVCODEC_LIBS)
		],
		[ AC_MSG_ERROR([libavcodec not found]) ])
PKG_CHECK_MODULES(AVUTIL, [ libavutil >= 5.0 ],
		[
			AC_SUBST(AVUTIL_CFLAGS)
			AC_SUBST(AVUTIL_LIBS)
		],
		[ AC_MSG_ERROR([libavutil not found]) ])

PKG_CHECK_MODULES(GStreamer,
		   [
			gstreamer-1.0 >= 1.0.0
			gstreamer-base-1.0 >= 1.0.0
			gstreamer-controller-1.0 >= 1.0.0
        	   ],
		   [
			AC_SUBST(GStreamer_CFLAGS)
			AC_SUBST(GStreamer_LIBS)
        	   ],
		   [
			AC_MSG_ERROR([Could not find GStreamer 1.0 libraries])
		   ])

PKG_CHECK_MODULES(LibXML2,
		   [
			libxml-2.0 >= 2.9.0
        	   ],
		   [
			AC_SUBST(LibXML2_CFLAGS)
			AC_SUBST(LibXML2_LIBS)
        	   ],
		   [
			AC_MSG_ERROR([Could not find libxml2])
		   ])

# Output files
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT


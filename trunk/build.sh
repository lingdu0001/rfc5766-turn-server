#!/bin/sh

SYSTEM=`uname`

GNUOSCFLAGS="-Wall -Wextra -Wformat-security -Wnested-externs -Wstrict-prototypes  -Wmissing-prototypes -Wpointer-arith -Wcast-qual -Wredundant-decls"

#GNUOSCFLAGS="-Werror -Winline ${GNUOSCFLAGS}"

OSCFLAGS=

if [ -z "${CC}" ] ; then
    CC=cc
else
    if [ "${CC}" = "gcc" ] ; then
	OSCFLAGS="${GNUOSCFLAGS}"
    else
	if [ "${CC}" = "clang" ] ; then
	    OSCFLAGS="${GNUOSCFLAGS}"
	fi
    fi
fi

if [ -z "${OSCFLAGS}" ] ; then  
    COMPSIGN=`${CC} --version 2>/dev/null | grep -i gcc | cut -b 1`
    RETRES=$?
    if [ ${RETRES} -eq 0 ] ; then
	if [ ${COMPSIGN} ] ; then
	    OSCFLAGS="${GNUOSCFLAGS}"
	fi
    fi
fi

if [ -z "${OSCFLAGS}" ] ; then  
    COMPSIGN=`${CC} --version 2>/dev/null | grep -i clang | cut -b 1`
    RETRES=$?
    if [ ${RETRES} -eq 0 ] ; then
	if [ ${COMPSIGN} ] ; then
	    OSCFLAGS="${GNUOSCFLAGS}"
	fi
    fi
fi

if [ -z "${OSCFLAGS}" ] ; then  
    COMPSIGN=`${CC} --version 2>/dev/null | grep -i "Free Software Foundation" | cut -b 1`
    RETRES=$?
    if [ ${RETRES} -eq 0 ] ; then
	if [ ${COMPSIGN} ] ; then
	    OSCFLAGS="${GNUOSCFLAGS}"
	fi
    fi
fi

if [ "${SYSTEM}" = "SunOS" ] ; then
# Solaris
    OSLIBS="-lsocket -lnsl -ldl"
    OSCFLAGS=${OSCFLAGS}" -D__EXTENSIONS__ -D_XOPEN_SOURCE=500"
fi

ISLINUX=`echo ${SYSTEM} | grep -i linux | cut -b 1`
if [ ${ISLINUX} ] ; then
#Linux
  OSLIBS="-ldl"
fi

ISBSD=`echo ${SYSTEM} | grep -i bsd | cut -b 1`
if [ ${ISBSD} ] ; then
#BSD
  OSLIBS=
fi

make OSLIBS="${OSLIBS}" OSCFLAGS="${OSCFLAGS}" $@

# $FreeBSD: src/sys/modules/if_gif/Makefile,v 1.11.2.1.2.1 2009/10/25 01:10:29 kensmith Exp $

.include <bsd.own.mk>

.PATH: ${.CURDIR}/../../net ${.CURDIR}/../../netinet ${.CURDIR}/../../netinet6

KMOD=	if_gif
SRCS=	if_gif.c in_gif.c opt_inet.h opt_inet6.h opt_mrouting.h

.if !defined(KERNBUILDDIR)
opt_inet.h:
	echo "#define INET 1" > ${.TARGET}

.if ${MK_INET6_SUPPORT} != "no"
opt_inet6.h:
	echo "#define INET6 1" > ${.TARGET}
.endif

opt_mrouting.h:
	echo "#define MROUTING 1" > ${.TARGET}
.else
OPT_INET6!= cat ${KERNBUILDDIR}/opt_inet6.h
.if empty(OPT_INET6)
MK_INET6_SUPPORT= no
.endif
.endif

.if ${MK_INET6_SUPPORT} != "no"
SRCS+=	in6_gif.c
.endif

.include <bsd.kmod.mk>

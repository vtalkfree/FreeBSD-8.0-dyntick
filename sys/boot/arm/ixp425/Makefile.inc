# $FreeBSD: src/sys/boot/arm/ixp425/Makefile.inc,v 1.1.2.1.2.1 2009/10/25 01:10:29 kensmith Exp $

.if !target(__ixp425_boot_Makefile.inc__)
.PATH:	${.CURDIR}/../../../../libkern ${.CURDIR}/../../../../libkern/arm

__ixp425_boot_Makefile.inc__:

# Both Avila and Pronghorn Metro are supported by ixp425
BOOT_FLAVOR=ixp425

CFLAGS+=-Os -ffreestanding \
	-I${.CURDIR}/../../../.. \
	-I${.CURDIR}/../../../../arm \
	-DCPU_XSCALE_IXP425 \
	-Wall -Waggregate-return  \
	-Werror \
	-Wnested-externs \
	-Wpointer-arith -Wshadow -Wwrite-strings \
	-Wmissing-prototypes \
	-Wmissing-declarations 

# -Wstrict-prototypes 

CFLAGS+=-DBOOT_${BOOT_FLAVOR:U}

LD ?= ld
OBJCOPY ?= objcopy

.if defined(P)
${P}: ${OBJS}
	${LD} ${LDFLAGS} -o ${.TARGET} ${OBJS}

CLEANFILES+= ${P}
.endif

.if defined(WITH_TAG_LIST)
MK_TAG_LIST:=yes
.else
MK_TAG_LIST:=no
.endif

.endif

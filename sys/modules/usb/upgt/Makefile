# $FreeBSD: src/sys/modules/usb/upgt/Makefile,v 1.1.2.1.2.1 2009/10/25 01:10:29 kensmith Exp $

.PATH: ${.CURDIR}/../../../dev/usb/wlan

KMOD    = if_upgt
SRCS    = if_upgt.c if_upgtvar.h	\
	  bus_if.h device_if.h \
	  opt_bus.h opt_usb.h usb_if.h usbdevs.h

.include <bsd.kmod.mk>

# Makefile for syscall tables
#
# $FreeBSD: src/sys/compat/svr4/Makefile,v 1.8.10.1.2.1 2009/10/25 01:10:29 kensmith Exp $

all:
	@echo "make sysent only"

sysent:  svr4_sysent.c svr4_syscall.h svr4_proto.h

svr4_syscallnames.c svr4_sysent.c svr4_syscall.h svr4_proto.h: \
		../../kern/makesyscalls.sh syscalls.master syscalls.conf
	-mv -f svr4_syscallnames.c svr4_syscallnames.c.bak
	-mv -f svr4_sysent.c svr4_sysent.c.bak
	-mv -f svr4_syscall.h svr4_syscall.h.bak
	-mv -f svr4_proto.h svr4_proto.h.bak
	sh ../../kern/makesyscalls.sh syscalls.master syscalls.conf


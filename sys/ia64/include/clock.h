/*-
 * Kernel interface to machine-dependent clock driver.
 * Garrett Wollman, September 1994.
 * This file is in the public domain.
 *
 * $FreeBSD: src/sys/ia64/include/clock.h,v 1.12.2.1.2.1 2009/10/25 01:10:29 kensmith Exp $
 */

#ifndef _MACHINE_CLOCK_H_
#define	_MACHINE_CLOCK_H_

#ifdef _KERNEL

#define	CLOCK_VECTOR	254

extern uint64_t	ia64_clock_reload;
extern uint64_t	itc_frequency;

#endif

#endif /* !_MACHINE_CLOCK_H_ */

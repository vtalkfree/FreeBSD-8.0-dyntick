/* $FreeBSD: src/sys/dev/cxgb/ulp/tom/cxgb_tcp_offload.h,v 1.7.2.1.2.1 2009/10/25 01:10:29 kensmith Exp $ */

#ifndef CXGB_TCP_OFFLOAD_H_
#define CXGB_TCP_OFFLOAD_H_

struct sockbuf;

void sockbuf_lock(struct sockbuf *);
void sockbuf_lock_assert(struct sockbuf *);
void sockbuf_unlock(struct sockbuf *);
int  sockbuf_sbspace(struct sockbuf *);


#endif /* CXGB_TCP_OFFLOAD_H_ */

#ifndef SHANE
#define SHANE

#include <rte_mbuf.h>                                                                                                   

extern
void (*shane_callback)(struct rte_mbuf **__rte_restrict pkts, unsigned int pkts_n);

extern
void setShaneCallback(void (*func)(struct rte_mbuf **__rte_restrict pkts, unsigned int pkts_n));

#endif

#include <shane.h>
#include <stdio.h>

void (*shane_callback)(struct rte_mbuf **__rte_restrict pkts, unsigned int pkts_n) = 0;

void setShaneCallback(void (*func)(struct rte_mbuf **__rte_restrict pkts, unsigned int pkts_n)) {
  shane_callback=func;
  printf("shane callback set\n");
}

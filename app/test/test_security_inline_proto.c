/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2022 Marvell.
 */


#include <stdio.h>
#include <inttypes.h>

#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_security.h>

#include "test.h"
#include "test_security_inline_proto_vectors.h"

#ifdef RTE_EXEC_ENV_WINDOWS
static int
test_inline_ipsec(void)
{
	printf("Inline ipsec not supported on Windows, skipping test\n");
	return TEST_SKIPPED;
}

#else

#define NB_ETHPORTS_USED		1
#define MEMPOOL_CACHE_SIZE		32
#define MAX_PKT_BURST			32
#define RTE_TEST_RX_DESC_DEFAULT	1024
#define RTE_TEST_TX_DESC_DEFAULT	1024
#define RTE_PORT_ALL		(~(uint16_t)0x0)

#define RX_PTHRESH 8 /**< Default values of RX prefetch threshold reg. */
#define RX_HTHRESH 8 /**< Default values of RX host threshold reg. */
#define RX_WTHRESH 0 /**< Default values of RX write-back threshold reg. */

#define TX_PTHRESH 32 /**< Default values of TX prefetch threshold reg. */
#define TX_HTHRESH 0  /**< Default values of TX host threshold reg. */
#define TX_WTHRESH 0  /**< Default values of TX write-back threshold reg. */

#define MAX_TRAFFIC_BURST		2048
#define NB_MBUF				10240

#define ENCAP_DECAP_BURST_SZ		33
#define APP_REASS_TIMEOUT		10

extern struct ipsec_test_data pkt_aes_128_gcm;
extern struct ipsec_test_data pkt_aes_192_gcm;
extern struct ipsec_test_data pkt_aes_256_gcm;
extern struct ipsec_test_data pkt_aes_128_gcm_frag;
extern struct ipsec_test_data pkt_aes_128_cbc_null;
extern struct ipsec_test_data pkt_null_aes_xcbc;
extern struct ipsec_test_data pkt_aes_128_cbc_hmac_sha384;
extern struct ipsec_test_data pkt_aes_128_cbc_hmac_sha512;

static struct rte_mempool *mbufpool;
static struct rte_mempool *sess_pool;
static struct rte_mempool *sess_priv_pool;
/* ethernet addresses of ports */
static struct rte_ether_addr ports_eth_addr[RTE_MAX_ETHPORTS];

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode = RTE_ETH_MQ_RX_NONE,
		.split_hdr_size = 0,
		.offloads = RTE_ETH_RX_OFFLOAD_CHECKSUM |
			    RTE_ETH_RX_OFFLOAD_SECURITY,
	},
	.txmode = {
		.mq_mode = RTE_ETH_MQ_TX_NONE,
		.offloads = RTE_ETH_TX_OFFLOAD_SECURITY |
			    RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE,
	},
	.lpbk_mode = 1,  /* enable loopback */
};

static struct rte_eth_rxconf rx_conf = {
	.rx_thresh = {
		.pthresh = RX_PTHRESH,
		.hthresh = RX_HTHRESH,
		.wthresh = RX_WTHRESH,
	},
	.rx_free_thresh = 32,
};

static struct rte_eth_txconf tx_conf = {
	.tx_thresh = {
		.pthresh = TX_PTHRESH,
		.hthresh = TX_HTHRESH,
		.wthresh = TX_WTHRESH,
	},
	.tx_free_thresh = 32, /* Use PMD default values */
	.tx_rs_thresh = 32, /* Use PMD default values */
};

uint16_t port_id;

static uint64_t link_mbps;

static int ip_reassembly_dynfield_offset = -1;

static struct rte_flow *default_flow[RTE_MAX_ETHPORTS];

/* Create Inline IPsec session */
static int
create_inline_ipsec_session(struct ipsec_test_data *sa, uint16_t portid,
		struct rte_security_session **sess, struct rte_security_ctx **ctx,
		uint32_t *ol_flags, const struct ipsec_test_flags *flags,
		struct rte_security_session_conf *sess_conf)
{
	uint16_t src_v6[8] = {0x2607, 0xf8b0, 0x400c, 0x0c03, 0x0000, 0x0000,
				0x0000, 0x001a};
	uint16_t dst_v6[8] = {0x2001, 0x0470, 0xe5bf, 0xdead, 0x4957, 0x2174,
				0xe82c, 0x4887};
	uint32_t src_v4 = rte_cpu_to_be_32(RTE_IPV4(192, 168, 1, 2));
	uint32_t dst_v4 = rte_cpu_to_be_32(RTE_IPV4(192, 168, 1, 1));
	struct rte_security_capability_idx sec_cap_idx;
	const struct rte_security_capability *sec_cap;
	enum rte_security_ipsec_sa_direction dir;
	struct rte_security_ctx *sec_ctx;
	uint32_t verify;

	sess_conf->action_type = RTE_SECURITY_ACTION_TYPE_INLINE_PROTOCOL;
	sess_conf->protocol = RTE_SECURITY_PROTOCOL_IPSEC;
	sess_conf->ipsec = sa->ipsec_xform;

	dir = sa->ipsec_xform.direction;
	verify = flags->tunnel_hdr_verify;

	if ((dir == RTE_SECURITY_IPSEC_SA_DIR_INGRESS) && verify) {
		if (verify == RTE_SECURITY_IPSEC_TUNNEL_VERIFY_SRC_DST_ADDR)
			src_v4 += 1;
		else if (verify == RTE_SECURITY_IPSEC_TUNNEL_VERIFY_DST_ADDR)
			dst_v4 += 1;
	}

	if (sa->ipsec_xform.mode == RTE_SECURITY_IPSEC_SA_MODE_TUNNEL) {
		if (sa->ipsec_xform.tunnel.type ==
				RTE_SECURITY_IPSEC_TUNNEL_IPV4) {
			memcpy(&sess_conf->ipsec.tunnel.ipv4.src_ip, &src_v4,
					sizeof(src_v4));
			memcpy(&sess_conf->ipsec.tunnel.ipv4.dst_ip, &dst_v4,
					sizeof(dst_v4));

			if (flags->df == TEST_IPSEC_SET_DF_0_INNER_1)
				sess_conf->ipsec.tunnel.ipv4.df = 0;

			if (flags->df == TEST_IPSEC_SET_DF_1_INNER_0)
				sess_conf->ipsec.tunnel.ipv4.df = 1;

			if (flags->dscp == TEST_IPSEC_SET_DSCP_0_INNER_1)
				sess_conf->ipsec.tunnel.ipv4.dscp = 0;

			if (flags->dscp == TEST_IPSEC_SET_DSCP_1_INNER_0)
				sess_conf->ipsec.tunnel.ipv4.dscp =
						TEST_IPSEC_DSCP_VAL;
		} else {
			if (flags->dscp == TEST_IPSEC_SET_DSCP_0_INNER_1)
				sess_conf->ipsec.tunnel.ipv6.dscp = 0;

			if (flags->dscp == TEST_IPSEC_SET_DSCP_1_INNER_0)
				sess_conf->ipsec.tunnel.ipv6.dscp =
						TEST_IPSEC_DSCP_VAL;

			memcpy(&sess_conf->ipsec.tunnel.ipv6.src_addr, &src_v6,
					sizeof(src_v6));
			memcpy(&sess_conf->ipsec.tunnel.ipv6.dst_addr, &dst_v6,
					sizeof(dst_v6));
		}
	}

	/* Save SA as userdata for the security session. When
	 * the packet is received, this userdata will be
	 * retrieved using the metadata from the packet.
	 *
	 * The PMD is expected to set similar metadata for other
	 * operations, like rte_eth_event, which are tied to
	 * security session. In such cases, the userdata could
	 * be obtained to uniquely identify the security
	 * parameters denoted.
	 */

	sess_conf->userdata = (void *) sa;

	sec_ctx = (struct rte_security_ctx *)rte_eth_dev_get_sec_ctx(portid);
	if (sec_ctx == NULL) {
		printf("Ethernet device doesn't support security features.\n");
		return TEST_SKIPPED;
	}

	sec_cap_idx.action = RTE_SECURITY_ACTION_TYPE_INLINE_PROTOCOL;
	sec_cap_idx.protocol = RTE_SECURITY_PROTOCOL_IPSEC;
	sec_cap_idx.ipsec.proto = sess_conf->ipsec.proto;
	sec_cap_idx.ipsec.mode = sess_conf->ipsec.mode;
	sec_cap_idx.ipsec.direction = sess_conf->ipsec.direction;
	sec_cap = rte_security_capability_get(sec_ctx, &sec_cap_idx);
	if (sec_cap == NULL) {
		printf("No capabilities registered\n");
		return TEST_SKIPPED;
	}

	if (sa->aead || sa->aes_gmac)
		memcpy(&sess_conf->ipsec.salt, sa->salt.data,
			RTE_MIN(sizeof(sess_conf->ipsec.salt), sa->salt.len));

	/* Copy cipher session parameters */
	if (sa->aead) {
		rte_memcpy(sess_conf->crypto_xform, &sa->xform.aead,
				sizeof(struct rte_crypto_sym_xform));
		sess_conf->crypto_xform->aead.key.data = sa->key.data;
		/* Verify crypto capabilities */
		if (test_ipsec_crypto_caps_aead_verify(sec_cap,
					sess_conf->crypto_xform) != 0) {
			RTE_LOG(INFO, USER1,
				"Crypto capabilities not supported\n");
			return TEST_SKIPPED;
		}
	} else {
		if (dir == RTE_SECURITY_IPSEC_SA_DIR_EGRESS) {
			rte_memcpy(&sess_conf->crypto_xform->cipher,
					&sa->xform.chain.cipher.cipher,
					sizeof(struct rte_crypto_cipher_xform));

			rte_memcpy(&sess_conf->crypto_xform->next->auth,
					&sa->xform.chain.auth.auth,
					sizeof(struct rte_crypto_auth_xform));
			sess_conf->crypto_xform->cipher.key.data =
							sa->key.data;
			sess_conf->crypto_xform->next->auth.key.data =
							sa->auth_key.data;
			/* Verify crypto capabilities */
			if (test_ipsec_crypto_caps_cipher_verify(sec_cap,
					sess_conf->crypto_xform) != 0) {
				RTE_LOG(INFO, USER1,
					"Cipher crypto capabilities not supported\n");
				return TEST_SKIPPED;
			}

			if (test_ipsec_crypto_caps_auth_verify(sec_cap,
					sess_conf->crypto_xform->next) != 0) {
				RTE_LOG(INFO, USER1,
					"Auth crypto capabilities not supported\n");
				return TEST_SKIPPED;
			}
		} else {
			rte_memcpy(&sess_conf->crypto_xform->next->cipher,
					&sa->xform.chain.cipher.cipher,
					sizeof(struct rte_crypto_cipher_xform));
			rte_memcpy(&sess_conf->crypto_xform->auth,
					&sa->xform.chain.auth.auth,
					sizeof(struct rte_crypto_auth_xform));
			sess_conf->crypto_xform->auth.key.data =
							sa->auth_key.data;
			sess_conf->crypto_xform->next->cipher.key.data =
							sa->key.data;

			/* Verify crypto capabilities */
			if (test_ipsec_crypto_caps_cipher_verify(sec_cap,
					sess_conf->crypto_xform->next) != 0) {
				RTE_LOG(INFO, USER1,
					"Cipher crypto capabilities not supported\n");
				return TEST_SKIPPED;
			}

			if (test_ipsec_crypto_caps_auth_verify(sec_cap,
					sess_conf->crypto_xform) != 0) {
				RTE_LOG(INFO, USER1,
					"Auth crypto capabilities not supported\n");
				return TEST_SKIPPED;
			}
		}
	}

	if (test_ipsec_sec_caps_verify(&sess_conf->ipsec, sec_cap, false) != 0)
		return TEST_SKIPPED;

	if ((sa->ipsec_xform.direction ==
			RTE_SECURITY_IPSEC_SA_DIR_EGRESS) &&
			(sa->ipsec_xform.options.iv_gen_disable == 1)) {
		/* Set env variable when IV generation is disabled */
		char arr[128];
		int len = 0, j = 0;
		int iv_len = (sa->aead || sa->aes_gmac) ? 8 : 16;

		for (; j < iv_len; j++)
			len += snprintf(arr+len, sizeof(arr) - len,
					"0x%x, ", sa->iv.data[j]);
		setenv("ETH_SEC_IV_OVR", arr, 1);
	}

	*sess = rte_security_session_create(sec_ctx,
				sess_conf, sess_pool, sess_priv_pool);
	if (*sess == NULL) {
		printf("SEC Session init failed.\n");
		return TEST_FAILED;
	}

	*ol_flags = sec_cap->ol_flags;
	*ctx = sec_ctx;

	return 0;
}

/* Check the link status of all ports in up to 3s, and print them finally */
static void
check_all_ports_link_status(uint16_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 30 /* 3s (30 * 100ms) in total */
	uint16_t portid;
	uint8_t count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;
	int ret;
	char link_status[RTE_ETH_LINK_MAX_STR_LEN];

	printf("Checking link statuses...\n");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		all_ports_up = 1;
		for (portid = 0; portid < port_num; portid++) {
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			ret = rte_eth_link_get_nowait(portid, &link);
			if (ret < 0) {
				all_ports_up = 0;
				if (print_flag == 1)
					printf("Port %u link get failed: %s\n",
						portid, rte_strerror(-ret));
				continue;
			}

			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status && link_mbps == 0)
					link_mbps = link.link_speed;

				rte_eth_link_to_str(link_status,
					sizeof(link_status), &link);
				printf("Port %d %s\n", portid, link_status);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == RTE_ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1))
			print_flag = 1;
	}
}

static void
print_ethaddr(const char *name, const struct rte_ether_addr *eth_addr)
{
	char buf[RTE_ETHER_ADDR_FMT_SIZE];
	rte_ether_format_addr(buf, RTE_ETHER_ADDR_FMT_SIZE, eth_addr);
	printf("%s%s", name, buf);
}

static void
copy_buf_to_pkt_segs(const uint8_t *buf, unsigned int len,
		     struct rte_mbuf *pkt, unsigned int offset)
{
	unsigned int copied = 0;
	unsigned int copy_len;
	struct rte_mbuf *seg;
	void *seg_buf;

	seg = pkt;
	while (offset >= seg->data_len) {
		offset -= seg->data_len;
		seg = seg->next;
	}
	copy_len = seg->data_len - offset;
	seg_buf = rte_pktmbuf_mtod_offset(seg, char *, offset);
	while (len > copy_len) {
		rte_memcpy(seg_buf, buf + copied, (size_t) copy_len);
		len -= copy_len;
		copied += copy_len;
		seg = seg->next;
		seg_buf = rte_pktmbuf_mtod(seg, void *);
	}
	rte_memcpy(seg_buf, buf + copied, (size_t) len);
}

static inline struct rte_mbuf *
init_packet(struct rte_mempool *mp, const uint8_t *data, unsigned int len)
{
	struct rte_mbuf *pkt;

	pkt = rte_pktmbuf_alloc(mp);
	if (pkt == NULL)
		return NULL;
	if (((data[0] & 0xF0) >> 4) == IPVERSION) {
		rte_memcpy(rte_pktmbuf_append(pkt, RTE_ETHER_HDR_LEN),
				&dummy_ipv4_eth_hdr, RTE_ETHER_HDR_LEN);
		pkt->l3_len = sizeof(struct rte_ipv4_hdr);
	} else {
		rte_memcpy(rte_pktmbuf_append(pkt, RTE_ETHER_HDR_LEN),
				&dummy_ipv6_eth_hdr, RTE_ETHER_HDR_LEN);
		pkt->l3_len = sizeof(struct rte_ipv6_hdr);
	}
	pkt->l2_len = RTE_ETHER_HDR_LEN;

	if (pkt->buf_len > (len + RTE_ETHER_HDR_LEN))
		rte_memcpy(rte_pktmbuf_append(pkt, len), data, len);
	else
		copy_buf_to_pkt_segs(data, len, pkt, RTE_ETHER_HDR_LEN);
	return pkt;
}

static int
init_mempools(unsigned int nb_mbuf)
{
	struct rte_security_ctx *sec_ctx;
	uint16_t nb_sess = 512;
	uint32_t sess_sz;
	char s[64];

	if (mbufpool == NULL) {
		snprintf(s, sizeof(s), "mbuf_pool");
		mbufpool = rte_pktmbuf_pool_create(s, nb_mbuf,
				MEMPOOL_CACHE_SIZE, 0,
				RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);
		if (mbufpool == NULL) {
			printf("Cannot init mbuf pool\n");
			return TEST_FAILED;
		}
		printf("Allocated mbuf pool\n");
	}

	sec_ctx = rte_eth_dev_get_sec_ctx(port_id);
	if (sec_ctx == NULL) {
		printf("Device does not support Security ctx\n");
		return TEST_SKIPPED;
	}
	sess_sz = rte_security_session_get_size(sec_ctx);
	if (sess_pool == NULL) {
		snprintf(s, sizeof(s), "sess_pool");
		sess_pool = rte_mempool_create(s, nb_sess, sess_sz,
				MEMPOOL_CACHE_SIZE, 0,
				NULL, NULL, NULL, NULL,
				SOCKET_ID_ANY, 0);
		if (sess_pool == NULL) {
			printf("Cannot init sess pool\n");
			return TEST_FAILED;
		}
		printf("Allocated sess pool\n");
	}
	if (sess_priv_pool == NULL) {
		snprintf(s, sizeof(s), "sess_priv_pool");
		sess_priv_pool = rte_mempool_create(s, nb_sess, sess_sz,
				MEMPOOL_CACHE_SIZE, 0,
				NULL, NULL, NULL, NULL,
				SOCKET_ID_ANY, 0);
		if (sess_priv_pool == NULL) {
			printf("Cannot init sess_priv pool\n");
			return TEST_FAILED;
		}
		printf("Allocated sess_priv pool\n");
	}

	return 0;
}

static int
create_default_flow(uint16_t portid)
{
	struct rte_flow_action action[2];
	struct rte_flow_item pattern[2];
	struct rte_flow_attr attr = {0};
	struct rte_flow_error err;
	struct rte_flow *flow;
	int ret;

	/* Add the default rte_flow to enable SECURITY for all ESP packets */

	pattern[0].type = RTE_FLOW_ITEM_TYPE_ESP;
	pattern[0].spec = NULL;
	pattern[0].mask = NULL;
	pattern[0].last = NULL;
	pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

	action[0].type = RTE_FLOW_ACTION_TYPE_SECURITY;
	action[0].conf = NULL;
	action[1].type = RTE_FLOW_ACTION_TYPE_END;
	action[1].conf = NULL;

	attr.ingress = 1;

	ret = rte_flow_validate(portid, &attr, pattern, action, &err);
	if (ret) {
		printf("\nValidate flow failed, ret = %d\n", ret);
		return -1;
	}
	flow = rte_flow_create(portid, &attr, pattern, action, &err);
	if (flow == NULL) {
		printf("\nDefault flow rule create failed\n");
		return -1;
	}

	default_flow[portid] = flow;

	return 0;
}

static void
destroy_default_flow(uint16_t portid)
{
	struct rte_flow_error err;
	int ret;

	if (!default_flow[portid])
		return;
	ret = rte_flow_destroy(portid, default_flow[portid], &err);
	if (ret) {
		printf("\nDefault flow rule destroy failed\n");
		return;
	}
	default_flow[portid] = NULL;
}

struct rte_mbuf **tx_pkts_burst;
struct rte_mbuf **rx_pkts_burst;

static int
compare_pkt_data(struct rte_mbuf *m, uint8_t *ref, unsigned int tot_len)
{
	unsigned int len;
	unsigned int nb_segs = m->nb_segs;
	unsigned int matched = 0;
	struct rte_mbuf *save = m;

	while (m) {
		len = tot_len;
		if (len > m->data_len)
			len = m->data_len;
		if (len != 0) {
			if (memcmp(rte_pktmbuf_mtod(m, char *),
					ref + matched, len)) {
				printf("\n====Reassembly case failed: Data Mismatch");
				rte_hexdump(stdout, "Reassembled",
					rte_pktmbuf_mtod(m, char *),
					len);
				rte_hexdump(stdout, "reference",
					ref + matched,
					len);
				return TEST_FAILED;
			}
		}
		tot_len -= len;
		matched += len;
		m = m->next;
	}

	if (tot_len) {
		printf("\n====Reassembly case failed: Data Missing %u",
		       tot_len);
		printf("\n====nb_segs %u, tot_len %u", nb_segs, tot_len);
		rte_pktmbuf_dump(stderr, save, -1);
		return TEST_FAILED;
	}
	return TEST_SUCCESS;
}

static inline bool
is_ip_reassembly_incomplete(struct rte_mbuf *mbuf)
{
	static uint64_t ip_reassembly_dynflag;
	int ip_reassembly_dynflag_offset;

	if (ip_reassembly_dynflag == 0) {
		ip_reassembly_dynflag_offset = rte_mbuf_dynflag_lookup(
			RTE_MBUF_DYNFLAG_IP_REASSEMBLY_INCOMPLETE_NAME, NULL);
		if (ip_reassembly_dynflag_offset < 0)
			return false;
		ip_reassembly_dynflag = RTE_BIT64(ip_reassembly_dynflag_offset);
	}

	return (mbuf->ol_flags & ip_reassembly_dynflag) != 0;
}

static void
free_mbuf(struct rte_mbuf *mbuf)
{
	rte_eth_ip_reassembly_dynfield_t dynfield;

	if (!mbuf)
		return;

	if (!is_ip_reassembly_incomplete(mbuf)) {
		rte_pktmbuf_free(mbuf);
	} else {
		if (ip_reassembly_dynfield_offset < 0)
			return;

		while (mbuf) {
			dynfield = *RTE_MBUF_DYNFIELD(mbuf,
					ip_reassembly_dynfield_offset,
					rte_eth_ip_reassembly_dynfield_t *);
			rte_pktmbuf_free(mbuf);
			mbuf = dynfield.next_frag;
		}
	}
}


static int
get_and_verify_incomplete_frags(struct rte_mbuf *mbuf,
				struct reassembly_vector *vector)
{
	rte_eth_ip_reassembly_dynfield_t *dynfield[MAX_PKT_BURST];
	int j = 0, ret;
	/**
	 * IP reassembly offload is incomplete, and fragments are listed in
	 * dynfield which can be reassembled in SW.
	 */
	printf("\nHW IP Reassembly is not complete; attempt SW IP Reassembly,"
		"\nMatching with original frags.");

	if (ip_reassembly_dynfield_offset < 0)
		return -1;

	printf("\ncomparing frag: %d", j);
	/* Skip Ethernet header comparison */
	rte_pktmbuf_adj(mbuf, RTE_ETHER_HDR_LEN);
	ret = compare_pkt_data(mbuf, vector->frags[j]->data,
				vector->frags[j]->len);
	if (ret)
		return ret;
	j++;
	dynfield[j] = RTE_MBUF_DYNFIELD(mbuf, ip_reassembly_dynfield_offset,
					rte_eth_ip_reassembly_dynfield_t *);
	printf("\ncomparing frag: %d", j);
	/* Skip Ethernet header comparison */
	rte_pktmbuf_adj(dynfield[j]->next_frag, RTE_ETHER_HDR_LEN);
	ret = compare_pkt_data(dynfield[j]->next_frag, vector->frags[j]->data,
			vector->frags[j]->len);
	if (ret)
		return ret;

	while ((dynfield[j]->nb_frags > 1) &&
			is_ip_reassembly_incomplete(dynfield[j]->next_frag)) {
		j++;
		dynfield[j] = RTE_MBUF_DYNFIELD(dynfield[j-1]->next_frag,
					ip_reassembly_dynfield_offset,
					rte_eth_ip_reassembly_dynfield_t *);
		printf("\ncomparing frag: %d", j);
		/* Skip Ethernet header comparison */
		rte_pktmbuf_adj(dynfield[j]->next_frag, RTE_ETHER_HDR_LEN);
		ret = compare_pkt_data(dynfield[j]->next_frag,
				vector->frags[j]->data, vector->frags[j]->len);
		if (ret)
			return ret;
	}
	return ret;
}

static int
test_ipsec_with_reassembly(struct reassembly_vector *vector,
		const struct ipsec_test_flags *flags)
{
	struct rte_security_session *out_ses[ENCAP_DECAP_BURST_SZ] = {0};
	struct rte_security_session *in_ses[ENCAP_DECAP_BURST_SZ] = {0};
	struct rte_eth_ip_reassembly_params reass_capa = {0};
	struct rte_security_session_conf sess_conf_out = {0};
	struct rte_security_session_conf sess_conf_in = {0};
	unsigned int nb_tx, burst_sz, nb_sent = 0;
	struct rte_crypto_sym_xform cipher_out = {0};
	struct rte_crypto_sym_xform auth_out = {0};
	struct rte_crypto_sym_xform aead_out = {0};
	struct rte_crypto_sym_xform cipher_in = {0};
	struct rte_crypto_sym_xform auth_in = {0};
	struct rte_crypto_sym_xform aead_in = {0};
	struct ipsec_test_data sa_data;
	struct rte_security_ctx *ctx;
	unsigned int i, nb_rx = 0, j;
	uint32_t ol_flags;
	int ret = 0;

	burst_sz = vector->burst ? ENCAP_DECAP_BURST_SZ : 1;
	nb_tx = vector->nb_frags * burst_sz;

	rte_eth_dev_stop(port_id);
	if (ret != 0) {
		printf("rte_eth_dev_stop: err=%s, port=%u\n",
			       rte_strerror(-ret), port_id);
		return ret;
	}
	rte_eth_ip_reassembly_capability_get(port_id, &reass_capa);
	if (reass_capa.max_frags < vector->nb_frags)
		return TEST_SKIPPED;
	if (reass_capa.timeout_ms > APP_REASS_TIMEOUT) {
		reass_capa.timeout_ms = APP_REASS_TIMEOUT;
		rte_eth_ip_reassembly_conf_set(port_id, &reass_capa);
	}

	ret = rte_eth_dev_start(port_id);
	if (ret < 0) {
		printf("rte_eth_dev_start: err=%d, port=%d\n",
			ret, port_id);
		return ret;
	}

	memset(tx_pkts_burst, 0, sizeof(tx_pkts_burst[0]) * nb_tx);
	memset(rx_pkts_burst, 0, sizeof(rx_pkts_burst[0]) * nb_tx);

	for (i = 0; i < nb_tx; i += vector->nb_frags) {
		for (j = 0; j < vector->nb_frags; j++) {
			tx_pkts_burst[i+j] = init_packet(mbufpool,
						vector->frags[j]->data,
						vector->frags[j]->len);
			if (tx_pkts_burst[i+j] == NULL) {
				ret = -1;
				printf("\n packed init failed\n");
				goto out;
			}
		}
	}

	for (i = 0; i < burst_sz; i++) {
		memcpy(&sa_data, vector->sa_data,
				sizeof(struct ipsec_test_data));
		/* Update SPI for every new SA */
		sa_data.ipsec_xform.spi += i;
		sa_data.ipsec_xform.direction =
					RTE_SECURITY_IPSEC_SA_DIR_EGRESS;
		if (sa_data.aead) {
			sess_conf_out.crypto_xform = &aead_out;
		} else {
			sess_conf_out.crypto_xform = &cipher_out;
			sess_conf_out.crypto_xform->next = &auth_out;
		}

		/* Create Inline IPsec outbound session. */
		ret = create_inline_ipsec_session(&sa_data, port_id,
				&out_ses[i], &ctx, &ol_flags, flags,
				&sess_conf_out);
		if (ret) {
			printf("\nInline outbound session create failed\n");
			goto out;
		}
	}

	j = 0;
	for (i = 0; i < nb_tx; i++) {
		if (ol_flags & RTE_SECURITY_TX_OLOAD_NEED_MDATA)
			rte_security_set_pkt_metadata(ctx,
				out_ses[j], tx_pkts_burst[i], NULL);
		tx_pkts_burst[i]->ol_flags |= RTE_MBUF_F_TX_SEC_OFFLOAD;

		/* Move to next SA after nb_frags */
		if ((i + 1) % vector->nb_frags == 0)
			j++;
	}

	for (i = 0; i < burst_sz; i++) {
		memcpy(&sa_data, vector->sa_data,
				sizeof(struct ipsec_test_data));
		/* Update SPI for every new SA */
		sa_data.ipsec_xform.spi += i;
		sa_data.ipsec_xform.direction =
					RTE_SECURITY_IPSEC_SA_DIR_INGRESS;

		if (sa_data.aead) {
			sess_conf_in.crypto_xform = &aead_in;
		} else {
			sess_conf_in.crypto_xform = &auth_in;
			sess_conf_in.crypto_xform->next = &cipher_in;
		}
		/* Create Inline IPsec inbound session. */
		ret = create_inline_ipsec_session(&sa_data, port_id, &in_ses[i],
				&ctx, &ol_flags, flags, &sess_conf_in);
		if (ret) {
			printf("\nInline inbound session create failed\n");
			goto out;
		}
	}

	/* Retrieve reassembly dynfield offset if available */
	if (ip_reassembly_dynfield_offset < 0 && vector->nb_frags > 1)
		ip_reassembly_dynfield_offset = rte_mbuf_dynfield_lookup(
				RTE_MBUF_DYNFIELD_IP_REASSEMBLY_NAME, NULL);


	ret = create_default_flow(port_id);
	if (ret)
		goto out;

	nb_sent = rte_eth_tx_burst(port_id, 0, tx_pkts_burst, nb_tx);
	if (nb_sent != nb_tx) {
		ret = -1;
		printf("\nFailed to tx %u pkts", nb_tx);
		goto out;
	}

	rte_delay_ms(1);

	/* Retry few times before giving up */
	nb_rx = 0;
	j = 0;
	do {
		nb_rx += rte_eth_rx_burst(port_id, 0, &rx_pkts_burst[nb_rx],
					  nb_tx - nb_rx);
		j++;
		if (nb_rx >= nb_tx)
			break;
		rte_delay_ms(1);
	} while (j < 5 || !nb_rx);

	/* Check for minimum number of Rx packets expected */
	if ((vector->nb_frags == 1 && nb_rx != nb_tx) ||
	    (vector->nb_frags > 1 && nb_rx < burst_sz)) {
		printf("\nreceived less Rx pkts(%u) pkts\n", nb_rx);
		ret = TEST_FAILED;
		goto out;
	}

	for (i = 0; i < nb_rx; i++) {
		if (vector->nb_frags > 1 &&
		    is_ip_reassembly_incomplete(rx_pkts_burst[i])) {
			ret = get_and_verify_incomplete_frags(rx_pkts_burst[i],
							      vector);
			if (ret != TEST_SUCCESS)
				break;
			continue;
		}

		if (rx_pkts_burst[i]->ol_flags &
		    RTE_MBUF_F_RX_SEC_OFFLOAD_FAILED ||
		    !(rx_pkts_burst[i]->ol_flags & RTE_MBUF_F_RX_SEC_OFFLOAD)) {
			printf("\nsecurity offload failed\n");
			ret = TEST_FAILED;
			break;
		}

		if (vector->full_pkt->len + RTE_ETHER_HDR_LEN !=
				rx_pkts_burst[i]->pkt_len) {
			printf("\nreassembled/decrypted packet length mismatch\n");
			ret = TEST_FAILED;
			break;
		}
		rte_pktmbuf_adj(rx_pkts_burst[i], RTE_ETHER_HDR_LEN);
		ret = compare_pkt_data(rx_pkts_burst[i],
				       vector->full_pkt->data,
				       vector->full_pkt->len);
		if (ret != TEST_SUCCESS)
			break;
	}

out:
	destroy_default_flow(port_id);

	/* Clear session data. */
	for (i = 0; i < burst_sz; i++) {
		if (out_ses[i])
			rte_security_session_destroy(ctx, out_ses[i]);
		if (in_ses[i])
			rte_security_session_destroy(ctx, in_ses[i]);
	}

	for (i = nb_sent; i < nb_tx; i++)
		free_mbuf(tx_pkts_burst[i]);
	for (i = 0; i < nb_rx; i++)
		free_mbuf(rx_pkts_burst[i]);
	return ret;
}

static int
test_ipsec_inline_proto_process(struct ipsec_test_data *td,
		struct ipsec_test_data *res_d,
		int nb_pkts,
		bool silent,
		const struct ipsec_test_flags *flags)
{
	struct rte_security_session_conf sess_conf = {0};
	struct rte_crypto_sym_xform cipher = {0};
	struct rte_crypto_sym_xform auth = {0};
	struct rte_crypto_sym_xform aead = {0};
	struct rte_security_session *ses;
	struct rte_security_ctx *ctx;
	int nb_rx = 0, nb_sent;
	uint32_t ol_flags;
	int i, j = 0, ret;

	memset(rx_pkts_burst, 0, sizeof(rx_pkts_burst[0]) * nb_pkts);

	if (td->aead) {
		sess_conf.crypto_xform = &aead;
	} else {
		if (td->ipsec_xform.direction ==
				RTE_SECURITY_IPSEC_SA_DIR_EGRESS) {
			sess_conf.crypto_xform = &cipher;
			sess_conf.crypto_xform->type = RTE_CRYPTO_SYM_XFORM_CIPHER;
			sess_conf.crypto_xform->next = &auth;
			sess_conf.crypto_xform->next->type = RTE_CRYPTO_SYM_XFORM_AUTH;
		} else {
			sess_conf.crypto_xform = &auth;
			sess_conf.crypto_xform->type = RTE_CRYPTO_SYM_XFORM_AUTH;
			sess_conf.crypto_xform->next = &cipher;
			sess_conf.crypto_xform->next->type = RTE_CRYPTO_SYM_XFORM_CIPHER;
		}
	}

	/* Create Inline IPsec session. */
	ret = create_inline_ipsec_session(td, port_id, &ses, &ctx,
					  &ol_flags, flags, &sess_conf);
	if (ret)
		return ret;

	if (td->ipsec_xform.direction == RTE_SECURITY_IPSEC_SA_DIR_INGRESS) {
		ret = create_default_flow(port_id);
		if (ret)
			goto out;
	}
	for (i = 0; i < nb_pkts; i++) {
		tx_pkts_burst[i] = init_packet(mbufpool, td->input_text.data,
						td->input_text.len);
		if (tx_pkts_burst[i] == NULL) {
			while (i--)
				rte_pktmbuf_free(tx_pkts_burst[i]);
			ret = TEST_FAILED;
			goto out;
		}

		if (test_ipsec_pkt_update(rte_pktmbuf_mtod_offset(tx_pkts_burst[i],
					uint8_t *, RTE_ETHER_HDR_LEN), flags)) {
			while (i--)
				rte_pktmbuf_free(tx_pkts_burst[i]);
			ret = TEST_FAILED;
			goto out;
		}

		if (td->ipsec_xform.direction == RTE_SECURITY_IPSEC_SA_DIR_EGRESS) {
			if (ol_flags & RTE_SECURITY_TX_OLOAD_NEED_MDATA)
				rte_security_set_pkt_metadata(ctx, ses,
						tx_pkts_burst[i], NULL);
			tx_pkts_burst[i]->ol_flags |= RTE_MBUF_F_TX_SEC_OFFLOAD;
		}
	}
	/* Send packet to ethdev for inline IPsec processing. */
	nb_sent = rte_eth_tx_burst(port_id, 0, tx_pkts_burst, nb_pkts);
	if (nb_sent != nb_pkts) {
		printf("\nUnable to TX %d packets", nb_pkts);
		for ( ; nb_sent < nb_pkts; nb_sent++)
			rte_pktmbuf_free(tx_pkts_burst[nb_sent]);
		ret = TEST_FAILED;
		goto out;
	}

	rte_pause();

	/* Receive back packet on loopback interface. */
	do {
		rte_delay_ms(1);
		nb_rx += rte_eth_rx_burst(port_id, 0, &rx_pkts_burst[nb_rx],
				nb_sent - nb_rx);
		if (nb_rx >= nb_sent)
			break;
	} while (j++ < 5 || nb_rx == 0);

	if (nb_rx != nb_sent) {
		printf("\nUnable to RX all %d packets", nb_sent);
		while (--nb_rx)
			rte_pktmbuf_free(rx_pkts_burst[nb_rx]);
		ret = TEST_FAILED;
		goto out;
	}

	for (i = 0; i < nb_rx; i++) {
		rte_pktmbuf_adj(rx_pkts_burst[i], RTE_ETHER_HDR_LEN);

		ret = test_ipsec_post_process(rx_pkts_burst[i], td,
					      res_d, silent, flags);
		if (ret != TEST_SUCCESS) {
			for ( ; i < nb_rx; i++)
				rte_pktmbuf_free(rx_pkts_burst[i]);
			goto out;
		}

		ret = test_ipsec_stats_verify(ctx, ses, flags,
					td->ipsec_xform.direction);
		if (ret != TEST_SUCCESS) {
			for ( ; i < nb_rx; i++)
				rte_pktmbuf_free(rx_pkts_burst[i]);
			goto out;
		}

		rte_pktmbuf_free(rx_pkts_burst[i]);
		rx_pkts_burst[i] = NULL;
	}

out:
	if (td->ipsec_xform.direction == RTE_SECURITY_IPSEC_SA_DIR_INGRESS)
		destroy_default_flow(port_id);

	/* Destroy session so that other cases can create the session again */
	rte_security_session_destroy(ctx, ses);
	ses = NULL;

	return ret;
}

static int
test_ipsec_inline_proto_all(const struct ipsec_test_flags *flags)
{
	struct ipsec_test_data td_outb;
	struct ipsec_test_data td_inb;
	unsigned int i, nb_pkts = 1, pass_cnt = 0, fail_cnt = 0;
	int ret;

	if (flags->iv_gen || flags->sa_expiry_pkts_soft ||
			flags->sa_expiry_pkts_hard)
		nb_pkts = IPSEC_TEST_PACKETS_MAX;

	for (i = 0; i < RTE_DIM(alg_list); i++) {
		test_ipsec_td_prepare(alg_list[i].param1,
				      alg_list[i].param2,
				      flags, &td_outb, 1);

		if (!td_outb.aead) {
			enum rte_crypto_cipher_algorithm cipher_alg;
			enum rte_crypto_auth_algorithm auth_alg;

			cipher_alg = td_outb.xform.chain.cipher.cipher.algo;
			auth_alg = td_outb.xform.chain.auth.auth.algo;

			if (td_outb.aes_gmac && cipher_alg != RTE_CRYPTO_CIPHER_NULL)
				continue;

			/* ICV is not applicable for NULL auth */
			if (flags->icv_corrupt &&
			    auth_alg == RTE_CRYPTO_AUTH_NULL)
				continue;

			/* IV is not applicable for NULL cipher */
			if (flags->iv_gen &&
			    cipher_alg == RTE_CRYPTO_CIPHER_NULL)
				continue;
		}

		if (flags->udp_encap)
			td_outb.ipsec_xform.options.udp_encap = 1;

		ret = test_ipsec_inline_proto_process(&td_outb, &td_inb, nb_pkts,
						false, flags);
		if (ret == TEST_SKIPPED)
			continue;

		if (ret == TEST_FAILED) {
			printf("\n TEST FAILED");
			test_ipsec_display_alg(alg_list[i].param1,
					       alg_list[i].param2);
			fail_cnt++;
			continue;
		}

		test_ipsec_td_update(&td_inb, &td_outb, 1, flags);

		ret = test_ipsec_inline_proto_process(&td_inb, NULL, nb_pkts,
						false, flags);
		if (ret == TEST_SKIPPED)
			continue;

		if (ret == TEST_FAILED) {
			printf("\n TEST FAILED");
			test_ipsec_display_alg(alg_list[i].param1,
					       alg_list[i].param2);
			fail_cnt++;
			continue;
		}

		if (flags->display_alg)
			test_ipsec_display_alg(alg_list[i].param1,
					       alg_list[i].param2);

		pass_cnt++;
	}

	printf("Tests passed: %d, failed: %d", pass_cnt, fail_cnt);
	if (fail_cnt > 0)
		return TEST_FAILED;
	if (pass_cnt > 0)
		return TEST_SUCCESS;
	else
		return TEST_SKIPPED;
}


static int
ut_setup_inline_ipsec(void)
{
	int ret;

	/* Start device */
	ret = rte_eth_dev_start(port_id);
	if (ret < 0) {
		printf("rte_eth_dev_start: err=%d, port=%d\n",
			ret, port_id);
		return ret;
	}
	/* always enable promiscuous */
	ret = rte_eth_promiscuous_enable(port_id);
	if (ret != 0) {
		printf("rte_eth_promiscuous_enable: err=%s, port=%d\n",
			rte_strerror(-ret), port_id);
		return ret;
	}

	check_all_ports_link_status(1, RTE_PORT_ALL);

	return 0;
}

static void
ut_teardown_inline_ipsec(void)
{
	struct rte_eth_ip_reassembly_params reass_conf = {0};
	uint16_t portid;
	int ret;

	/* port tear down */
	RTE_ETH_FOREACH_DEV(portid) {
		ret = rte_eth_dev_stop(portid);
		if (ret != 0)
			printf("rte_eth_dev_stop: err=%s, port=%u\n",
			       rte_strerror(-ret), portid);

		/* Clear reassembly configuration */
		rte_eth_ip_reassembly_conf_set(portid, &reass_conf);
	}
}

static int
inline_ipsec_testsuite_setup(void)
{
	uint16_t nb_rxd;
	uint16_t nb_txd;
	uint16_t nb_ports;
	int ret;
	uint16_t nb_rx_queue = 1, nb_tx_queue = 1;

	printf("Start inline IPsec test.\n");

	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports < NB_ETHPORTS_USED) {
		printf("At least %u port(s) used for test\n",
		       NB_ETHPORTS_USED);
		return TEST_SKIPPED;
	}

	ret = init_mempools(NB_MBUF);
	if (ret)
		return ret;

	if (tx_pkts_burst == NULL) {
		tx_pkts_burst = (struct rte_mbuf **)rte_calloc("tx_buff",
					  MAX_TRAFFIC_BURST,
					  sizeof(void *),
					  RTE_CACHE_LINE_SIZE);
		if (!tx_pkts_burst)
			return TEST_FAILED;

		rx_pkts_burst = (struct rte_mbuf **)rte_calloc("rx_buff",
					  MAX_TRAFFIC_BURST,
					  sizeof(void *),
					  RTE_CACHE_LINE_SIZE);
		if (!rx_pkts_burst)
			return TEST_FAILED;
	}

	printf("Generate %d packets\n", MAX_TRAFFIC_BURST);

	nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
	nb_txd = RTE_TEST_TX_DESC_DEFAULT;

	/* configuring port 0 for the test is enough */
	port_id = 0;
	/* port configure */
	ret = rte_eth_dev_configure(port_id, nb_rx_queue,
				    nb_tx_queue, &port_conf);
	if (ret < 0) {
		printf("Cannot configure device: err=%d, port=%d\n",
			 ret, port_id);
		return ret;
	}
	ret = rte_eth_macaddr_get(port_id, &ports_eth_addr[port_id]);
	if (ret < 0) {
		printf("Cannot get mac address: err=%d, port=%d\n",
			 ret, port_id);
		return ret;
	}
	printf("Port %u ", port_id);
	print_ethaddr("Address:", &ports_eth_addr[port_id]);
	printf("\n");

	/* tx queue setup */
	ret = rte_eth_tx_queue_setup(port_id, 0, nb_txd,
				     SOCKET_ID_ANY, &tx_conf);
	if (ret < 0) {
		printf("rte_eth_tx_queue_setup: err=%d, port=%d\n",
				ret, port_id);
		return ret;
	}
	/* rx queue steup */
	ret = rte_eth_rx_queue_setup(port_id, 0, nb_rxd, SOCKET_ID_ANY,
				     &rx_conf, mbufpool);
	if (ret < 0) {
		printf("rte_eth_rx_queue_setup: err=%d, port=%d\n",
				ret, port_id);
		return ret;
	}
	test_ipsec_alg_list_populate();

	return 0;
}

static void
inline_ipsec_testsuite_teardown(void)
{
	uint16_t portid;
	int ret;

	/* port tear down */
	RTE_ETH_FOREACH_DEV(portid) {
		ret = rte_eth_dev_reset(portid);
		if (ret != 0)
			printf("rte_eth_dev_reset: err=%s, port=%u\n",
			       rte_strerror(-ret), port_id);
	}
}

static int
test_inline_ip_reassembly(const void *testdata)
{
	struct reassembly_vector reassembly_td = {0};
	const struct reassembly_vector *td = testdata;
	struct ip_reassembly_test_packet full_pkt;
	struct ip_reassembly_test_packet frags[MAX_FRAGS];
	struct ipsec_test_flags flags = {0};
	int i = 0;

	reassembly_td.sa_data = td->sa_data;
	reassembly_td.nb_frags = td->nb_frags;
	reassembly_td.burst = td->burst;

	memcpy(&full_pkt, td->full_pkt,
			sizeof(struct ip_reassembly_test_packet));
	reassembly_td.full_pkt = &full_pkt;

	test_vector_payload_populate(reassembly_td.full_pkt, true);
	for (; i < reassembly_td.nb_frags; i++) {
		memcpy(&frags[i], td->frags[i],
			sizeof(struct ip_reassembly_test_packet));
		reassembly_td.frags[i] = &frags[i];
		test_vector_payload_populate(reassembly_td.frags[i],
				(i == 0) ? true : false);
	}

	return test_ipsec_with_reassembly(&reassembly_td, &flags);
}

static int
test_ipsec_inline_proto_known_vec(const void *test_data)
{
	struct ipsec_test_data td_outb;
	struct ipsec_test_flags flags;

	memset(&flags, 0, sizeof(flags));

	memcpy(&td_outb, test_data, sizeof(td_outb));

	if (td_outb.aead ||
	    td_outb.xform.chain.cipher.cipher.algo != RTE_CRYPTO_CIPHER_NULL) {
		/* Disable IV gen to be able to test with known vectors */
		td_outb.ipsec_xform.options.iv_gen_disable = 1;
	}

	return test_ipsec_inline_proto_process(&td_outb, NULL, 1,
				false, &flags);
}

static int
test_ipsec_inline_proto_known_vec_inb(const void *test_data)
{
	const struct ipsec_test_data *td = test_data;
	struct ipsec_test_flags flags;
	struct ipsec_test_data td_inb;

	memset(&flags, 0, sizeof(flags));

	if (td->ipsec_xform.direction == RTE_SECURITY_IPSEC_SA_DIR_EGRESS)
		test_ipsec_td_in_from_out(td, &td_inb);
	else
		memcpy(&td_inb, td, sizeof(td_inb));

	return test_ipsec_inline_proto_process(&td_inb, NULL, 1, false, &flags);
}

static int
test_ipsec_inline_proto_display_list(const void *data __rte_unused)
{
	struct ipsec_test_flags flags;

	memset(&flags, 0, sizeof(flags));

	flags.display_alg = true;

	return test_ipsec_inline_proto_all(&flags);
}

static struct unit_test_suite inline_ipsec_testsuite  = {
	.suite_name = "Inline IPsec Ethernet Device Unit Test Suite",
	.setup = inline_ipsec_testsuite_setup,
	.teardown = inline_ipsec_testsuite_teardown,
	.unit_test_cases = {
		TEST_CASE_NAMED_WITH_DATA(
			"Outbound known vector (ESP tunnel mode IPv4 AES-GCM 128)",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec, &pkt_aes_128_gcm),
		TEST_CASE_NAMED_WITH_DATA(
			"Outbound known vector (ESP tunnel mode IPv4 AES-GCM 192)",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec, &pkt_aes_192_gcm),
		TEST_CASE_NAMED_WITH_DATA(
			"Outbound known vector (ESP tunnel mode IPv4 AES-GCM 256)",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec, &pkt_aes_256_gcm),
		TEST_CASE_NAMED_WITH_DATA(
			"Outbound known vector (ESP tunnel mode IPv4 AES-CBC 128 HMAC-SHA256 [16B ICV])",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec,
			&pkt_aes_128_cbc_hmac_sha256),
		TEST_CASE_NAMED_WITH_DATA(
			"Outbound known vector (ESP tunnel mode IPv4 AES-CBC 128 HMAC-SHA384 [24B ICV])",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec,
			&pkt_aes_128_cbc_hmac_sha384),
		TEST_CASE_NAMED_WITH_DATA(
			"Outbound known vector (ESP tunnel mode IPv4 AES-CBC 128 HMAC-SHA512 [32B ICV])",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec,
			&pkt_aes_128_cbc_hmac_sha512),
		TEST_CASE_NAMED_WITH_DATA(
			"Outbound known vector (ESP tunnel mode IPv6 AES-GCM 128)",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec, &pkt_aes_256_gcm_v6),
		TEST_CASE_NAMED_WITH_DATA(
			"Outbound known vector (ESP tunnel mode IPv6 AES-CBC 128 HMAC-SHA256 [16B ICV])",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec,
			&pkt_aes_128_cbc_hmac_sha256_v6),
		TEST_CASE_NAMED_WITH_DATA(
			"Outbound known vector (ESP tunnel mode IPv4 NULL AES-XCBC-MAC [12B ICV])",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec,
			&pkt_null_aes_xcbc),
		TEST_CASE_NAMED_WITH_DATA(
			"Inbound known vector (ESP tunnel mode IPv4 AES-GCM 128)",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec_inb, &pkt_aes_128_gcm),
		TEST_CASE_NAMED_WITH_DATA(
			"Inbound known vector (ESP tunnel mode IPv4 AES-GCM 192)",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec_inb, &pkt_aes_192_gcm),
		TEST_CASE_NAMED_WITH_DATA(
			"Inbound known vector (ESP tunnel mode IPv4 AES-GCM 256)",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec_inb, &pkt_aes_256_gcm),
		TEST_CASE_NAMED_WITH_DATA(
			"Inbound known vector (ESP tunnel mode IPv4 AES-CBC 128)",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec_inb, &pkt_aes_128_cbc_null),
		TEST_CASE_NAMED_WITH_DATA(
			"Inbound known vector (ESP tunnel mode IPv4 AES-CBC 128 HMAC-SHA256 [16B ICV])",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec_inb,
			&pkt_aes_128_cbc_hmac_sha256),
		TEST_CASE_NAMED_WITH_DATA(
			"Inbound known vector (ESP tunnel mode IPv4 AES-CBC 128 HMAC-SHA384 [24B ICV])",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec_inb,
			&pkt_aes_128_cbc_hmac_sha384),
		TEST_CASE_NAMED_WITH_DATA(
			"Inbound known vector (ESP tunnel mode IPv4 AES-CBC 128 HMAC-SHA512 [32B ICV])",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec_inb,
			&pkt_aes_128_cbc_hmac_sha512),
		TEST_CASE_NAMED_WITH_DATA(
			"Inbound known vector (ESP tunnel mode IPv6 AES-GCM 128)",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec_inb, &pkt_aes_256_gcm_v6),
		TEST_CASE_NAMED_WITH_DATA(
			"Inbound known vector (ESP tunnel mode IPv6 AES-CBC 128 HMAC-SHA256 [16B ICV])",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec_inb,
			&pkt_aes_128_cbc_hmac_sha256_v6),
		TEST_CASE_NAMED_WITH_DATA(
			"Inbound known vector (ESP tunnel mode IPv4 NULL AES-XCBC-MAC [12B ICV])",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_known_vec_inb,
			&pkt_null_aes_xcbc),

		TEST_CASE_NAMED_ST(
			"Combined test alg list",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_ipsec_inline_proto_display_list),

		TEST_CASE_NAMED_WITH_DATA(
			"IPv4 Reassembly with 2 fragments",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_inline_ip_reassembly, &ipv4_2frag_vector),
		TEST_CASE_NAMED_WITH_DATA(
			"IPv6 Reassembly with 2 fragments",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_inline_ip_reassembly, &ipv6_2frag_vector),
		TEST_CASE_NAMED_WITH_DATA(
			"IPv4 Reassembly with 4 fragments",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_inline_ip_reassembly, &ipv4_4frag_vector),
		TEST_CASE_NAMED_WITH_DATA(
			"IPv6 Reassembly with 4 fragments",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_inline_ip_reassembly, &ipv6_4frag_vector),
		TEST_CASE_NAMED_WITH_DATA(
			"IPv4 Reassembly with 5 fragments",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_inline_ip_reassembly, &ipv4_5frag_vector),
		TEST_CASE_NAMED_WITH_DATA(
			"IPv6 Reassembly with 5 fragments",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_inline_ip_reassembly, &ipv6_5frag_vector),
		TEST_CASE_NAMED_WITH_DATA(
			"IPv4 Reassembly with incomplete fragments",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_inline_ip_reassembly, &ipv4_incomplete_vector),
		TEST_CASE_NAMED_WITH_DATA(
			"IPv4 Reassembly with overlapping fragments",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_inline_ip_reassembly, &ipv4_overlap_vector),
		TEST_CASE_NAMED_WITH_DATA(
			"IPv4 Reassembly with out of order fragments",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_inline_ip_reassembly, &ipv4_out_of_order_vector),
		TEST_CASE_NAMED_WITH_DATA(
			"IPv4 Reassembly with burst of 4 fragments",
			ut_setup_inline_ipsec, ut_teardown_inline_ipsec,
			test_inline_ip_reassembly, &ipv4_4frag_burst_vector),

		TEST_CASES_END() /**< NULL terminate unit test array */
	},
};


static int
test_inline_ipsec(void)
{
	return unit_test_suite_runner(&inline_ipsec_testsuite);
}

#endif /* !RTE_EXEC_ENV_WINDOWS */

REGISTER_TEST_COMMAND(inline_ipsec_autotest, test_inline_ipsec);
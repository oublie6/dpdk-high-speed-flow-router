#include "dp_action.h"

#include <errno.h>
#include <netinet/in.h>

#include <rte_byteorder.h>
#include <rte_ip4.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_udp.h>

int
dp_apply_action(struct rte_mbuf *mbuf, const struct dp_packet_meta *meta,
                    const struct dp_rule_action *action)
{
    uint8_t *data;
    struct rte_ipv4_hdr *ipv4;
    void *l4;

    if (!mbuf || !meta || !action)
        return -EINVAL;
    if (action->type == DP_ACTION_DROP || action->type == DP_ACTION_FORWARD)
        return 0;
    if (action->type != DP_ACTION_REWRITE || action->rewrite_mask == 0)
        return -EINVAL;

    data = rte_pktmbuf_mtod(mbuf, uint8_t *);
    ipv4 = (struct rte_ipv4_hdr *)(data + meta->l2_len);
    l4 = data + meta->l4_offset;

    if (action->rewrite_mask & DP_REWRITE_SRC_IPV4)
        ipv4->src_addr = rte_cpu_to_be_32(action->src_ipv4);
    if (action->rewrite_mask & DP_REWRITE_DST_IPV4)
        ipv4->dst_addr = rte_cpu_to_be_32(action->dst_ipv4);

    if (meta->l4_proto == IPPROTO_TCP) {
        struct rte_tcp_hdr *tcp = l4;
        if (action->rewrite_mask & DP_REWRITE_SRC_PORT)
            tcp->src_port = rte_cpu_to_be_16(action->src_port);
        if (action->rewrite_mask & DP_REWRITE_DST_PORT)
            tcp->dst_port = rte_cpu_to_be_16(action->dst_port);
        tcp->cksum = 0;
        ipv4->hdr_checksum = 0;
        ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
        tcp->cksum = rte_ipv4_udptcp_cksum(ipv4, tcp);
        return 0;
    }
    if (meta->l4_proto == IPPROTO_UDP) {
        struct rte_udp_hdr *udp = l4;
        rte_be16_t ipv4_total_length;
        uint16_t udp_length;
        if (action->rewrite_mask & DP_REWRITE_SRC_PORT)
            udp->src_port = rte_cpu_to_be_16(action->src_port);
        if (action->rewrite_mask & DP_REWRITE_DST_PORT)
            udp->dst_port = rte_cpu_to_be_16(action->dst_port);
        udp->dgram_cksum = 0;
        ipv4->hdr_checksum = 0;
        ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);

        /* Goal 003 允许 UDP length 小于 IPv4 payload。DPDK helper 从 IPv4
         * total_length 推导 L4 长度，因此计算 UDP checksum 时临时缩到真实
         * datagram 长度，随后恢复原始 header 字段。
         */
        ipv4_total_length = ipv4->total_length;
        udp_length = rte_be_to_cpu_16(udp->dgram_len);
        ipv4->total_length = rte_cpu_to_be_16(meta->l3_len + udp_length);
        udp->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4, udp);
        ipv4->total_length = ipv4_total_length;
        return 0;
    }
    return -EINVAL;
}

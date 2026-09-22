#include "dp_parser.h"

#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip4.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_udp.h>

static enum dp_parse_result
dp_parse_tcp(const uint8_t *data, uint16_t payload_len,
             struct dp_packet_meta *meta)
{
    const struct rte_tcp_hdr *tcp;
    uint8_t data_offset_words;
    uint16_t header_len;

    if (payload_len < sizeof(struct rte_tcp_hdr))
        return DP_PARSE_MALFORMED;

    tcp = (const struct rte_tcp_hdr *)data;
    data_offset_words = tcp->data_off >> 4;
    if (data_offset_words < 5)
        return DP_PARSE_MALFORMED;

    header_len = (uint16_t)data_offset_words * 4;
    if (header_len > payload_len)
        return DP_PARSE_MALFORMED;

    meta->src_port = rte_be_to_cpu_16(tcp->src_port);
    meta->dst_port = rte_be_to_cpu_16(tcp->dst_port);
    meta->l4_len = header_len;
    return DP_PARSE_OK;
}

static enum dp_parse_result
dp_parse_udp(const uint8_t *data, uint16_t payload_len,
             struct dp_packet_meta *meta)
{
    const struct rte_udp_hdr *udp;
    uint16_t datagram_len;

    if (payload_len < sizeof(struct rte_udp_hdr))
        return DP_PARSE_MALFORMED;

    udp = (const struct rte_udp_hdr *)data;
    datagram_len = rte_be_to_cpu_16(udp->dgram_len);
    if (datagram_len < sizeof(struct rte_udp_hdr) || datagram_len > payload_len)
        return DP_PARSE_MALFORMED;

    meta->src_port = rte_be_to_cpu_16(udp->src_port);
    meta->dst_port = rte_be_to_cpu_16(udp->dst_port);
    meta->l4_len = sizeof(struct rte_udp_hdr);
    return DP_PARSE_OK;
}

static enum dp_parse_result
dp_parse_ipv4(const uint8_t *data, uint16_t frame_len,
              struct dp_packet_meta *meta)
{
    const struct rte_ipv4_hdr *ipv4;
    uint8_t version;
    uint8_t ihl_words;
    uint16_t ihl_bytes;
    uint16_t total_len;
    uint16_t fragment;
    uint16_t payload_len;
    const uint8_t *l4;

    if (frame_len < meta->l2_len + sizeof(struct rte_ipv4_hdr))
        return DP_PARSE_MALFORMED;

    ipv4 = (const struct rte_ipv4_hdr *)(data + meta->l2_len);
    version = ipv4->version_ihl >> 4;
    ihl_words = ipv4->version_ihl & 0x0f;
    if (version != 4 || ihl_words < 5)
        return DP_PARSE_MALFORMED;

    ihl_bytes = (uint16_t)ihl_words * 4;
    if (ihl_bytes > frame_len - meta->l2_len)
        return DP_PARSE_MALFORMED;

    total_len = rte_be_to_cpu_16(ipv4->total_length);
    if (total_len < ihl_bytes || total_len > frame_len - meta->l2_len)
        return DP_PARSE_MALFORMED;

    fragment = rte_be_to_cpu_16(ipv4->fragment_offset);
    if ((fragment & (RTE_IPV4_HDR_MF_FLAG | RTE_IPV4_HDR_OFFSET_MASK)) != 0)
        return DP_PARSE_UNSUPPORTED;

    meta->src_ipv4 = rte_be_to_cpu_32(ipv4->src_addr);
    meta->dst_ipv4 = rte_be_to_cpu_32(ipv4->dst_addr);
    meta->l4_proto = ipv4->next_proto_id;
    meta->l3_len = ihl_bytes;
    meta->l4_offset = meta->l2_len + ihl_bytes;

    payload_len = total_len - ihl_bytes;
    l4 = data + meta->l4_offset;
    if (meta->l4_proto == IPPROTO_TCP)
        return dp_parse_tcp(l4, payload_len, meta);
    if (meta->l4_proto == IPPROTO_UDP)
        return dp_parse_udp(l4, payload_len, meta);
    return DP_PARSE_UNSUPPORTED;
}

enum dp_parse_result
dp_parse_packet(struct rte_mbuf *mbuf, struct dp_packet_meta *meta)
{
    const uint8_t *data;
    const struct rte_ether_hdr *ethernet;
    uint32_t packet_len;
    uint16_t data_len;

    if (!mbuf || !meta)
        return DP_PARSE_MALFORMED;

    /* 本阶段只支持一个连续 segment。先检查 nb_segs，避免后续读取跨段数据。 */
    if (mbuf->nb_segs != 1)
        return DP_PARSE_UNSUPPORTED;

    packet_len = rte_pktmbuf_pkt_len(mbuf);
    data_len = rte_pktmbuf_data_len(mbuf);
    if (packet_len != data_len || packet_len > UINT16_MAX)
        return DP_PARSE_MALFORMED;

    memset(meta, 0, sizeof(*meta));
    if (data_len < sizeof(struct rte_ether_hdr))
        return DP_PARSE_MALFORMED;

    data = rte_pktmbuf_mtod(mbuf, const uint8_t *);
    ethernet = (const struct rte_ether_hdr *)data;
    meta->ether_type = rte_be_to_cpu_16(ethernet->ether_type);
    meta->l2_len = sizeof(struct rte_ether_hdr);
    if (meta->ether_type != RTE_ETHER_TYPE_IPV4)
        return DP_PARSE_UNSUPPORTED;

    return dp_parse_ipv4(data, data_len, meta);
}

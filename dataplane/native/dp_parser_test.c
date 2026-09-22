#include "dp_parser.h"
#include "dp_test.h"

#include <errno.h>
#include <string.h>
#include <sys/types.h>

#include <rte_ether.h>
#include <rte_ip4.h>
#include <rte_mbuf.h>

#define DP_TEST_FRAME_CAPACITY 128
#define DP_TEST_L2_LEN 14
#define DP_TEST_IPV4_MIN_LEN 20
#define DP_TEST_TCP_MIN_LEN 20
#define DP_TEST_UDP_LEN 8

static void
dp_test_put_be16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void
dp_test_put_be32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static uint16_t
dp_test_build_tcp(uint8_t *frame)
{
    uint8_t *ipv4;
    uint8_t *tcp;
    uint16_t total_len = DP_TEST_IPV4_MIN_LEN + DP_TEST_TCP_MIN_LEN;

    memset(frame, 0, DP_TEST_FRAME_CAPACITY);
    dp_test_put_be16(frame + 12, RTE_ETHER_TYPE_IPV4);
    ipv4 = frame + DP_TEST_L2_LEN;
    ipv4[0] = 0x45;
    dp_test_put_be16(ipv4 + 2, total_len);
    ipv4[8] = 64;
    ipv4[9] = IPPROTO_TCP;
    dp_test_put_be32(ipv4 + 12, 0xc0000201);
    dp_test_put_be32(ipv4 + 16, 0xc6336402);
    tcp = ipv4 + DP_TEST_IPV4_MIN_LEN;
    dp_test_put_be16(tcp, 12345);
    dp_test_put_be16(tcp + 2, 443);
    tcp[12] = 0x50;
    return DP_TEST_L2_LEN + total_len;
}

static uint16_t
dp_test_build_udp(uint8_t *frame)
{
    uint8_t *ipv4;
    uint8_t *udp;
    uint16_t total_len = DP_TEST_IPV4_MIN_LEN + DP_TEST_UDP_LEN;

    memset(frame, 0, DP_TEST_FRAME_CAPACITY);
    dp_test_put_be16(frame + 12, RTE_ETHER_TYPE_IPV4);
    ipv4 = frame + DP_TEST_L2_LEN;
    ipv4[0] = 0x45;
    dp_test_put_be16(ipv4 + 2, total_len);
    ipv4[8] = 64;
    ipv4[9] = IPPROTO_UDP;
    dp_test_put_be32(ipv4 + 12, 0xc0000201);
    dp_test_put_be32(ipv4 + 16, 0xc6336402);
    udp = ipv4 + DP_TEST_IPV4_MIN_LEN;
    dp_test_put_be16(udp, 12345);
    dp_test_put_be16(udp + 2, 53);
    dp_test_put_be16(udp + 4, DP_TEST_UDP_LEN);
    return DP_TEST_L2_LEN + total_len;
}

static int
dp_test_prepare_fixture(int fixture, uint8_t *frame, uint16_t *frame_len,
                        uint16_t *nb_segs)
{
    uint8_t *ipv4;
    uint8_t *l4;

    *nb_segs = 1;
    if (fixture == DP_TEST_PARSE_VALID_UDP ||
        fixture == DP_TEST_PARSE_UDP_TRUNCATED ||
        fixture == DP_TEST_PARSE_INVALID_UDP_LENGTH ||
        fixture == DP_TEST_PARSE_UDP_BEYOND_PAYLOAD)
        *frame_len = dp_test_build_udp(frame);
    else
        *frame_len = dp_test_build_tcp(frame);

    ipv4 = frame + DP_TEST_L2_LEN;
    l4 = ipv4 + DP_TEST_IPV4_MIN_LEN;
    switch (fixture) {
    case DP_TEST_PARSE_VALID_TCP:
    case DP_TEST_PARSE_VALID_UDP:
        break;
    case DP_TEST_PARSE_IPV4_OPTIONS:
        memmove(ipv4 + 24, l4, DP_TEST_TCP_MIN_LEN);
        memset(ipv4 + 20, 0, 4);
        ipv4[0] = 0x46;
        dp_test_put_be16(ipv4 + 2, 24 + DP_TEST_TCP_MIN_LEN);
        *frame_len = DP_TEST_L2_LEN + 24 + DP_TEST_TCP_MIN_LEN;
        break;
    case DP_TEST_PARSE_TCP_OPTIONS:
        l4[12] = 0x60;
        dp_test_put_be16(ipv4 + 2, DP_TEST_IPV4_MIN_LEN + 24);
        *frame_len = DP_TEST_L2_LEN + DP_TEST_IPV4_MIN_LEN + 24;
        break;
    case DP_TEST_PARSE_NON_IPV4:
        dp_test_put_be16(frame + 12, RTE_ETHER_TYPE_IPV6);
        break;
    case DP_TEST_PARSE_UNSUPPORTED_L4:
        ipv4[9] = IPPROTO_ICMP;
        dp_test_put_be16(ipv4 + 2, DP_TEST_IPV4_MIN_LEN);
        *frame_len = DP_TEST_L2_LEN + DP_TEST_IPV4_MIN_LEN;
        break;
    case DP_TEST_PARSE_IPV4_FRAGMENT:
        dp_test_put_be16(ipv4 + 6, RTE_IPV4_HDR_MF_FLAG);
        break;
    case DP_TEST_PARSE_ETHERNET_TRUNCATED:
        *frame_len = DP_TEST_L2_LEN - 1;
        break;
    case DP_TEST_PARSE_IPV4_TRUNCATED:
        *frame_len = DP_TEST_L2_LEN + DP_TEST_IPV4_MIN_LEN - 1;
        break;
    case DP_TEST_PARSE_INVALID_IPV4_VERSION:
        ipv4[0] = 0x65;
        break;
    case DP_TEST_PARSE_INVALID_IHL:
        ipv4[0] = 0x44;
        break;
    case DP_TEST_PARSE_INVALID_TOTAL_LENGTH:
        dp_test_put_be16(ipv4 + 2, DP_TEST_IPV4_MIN_LEN - 1);
        break;
    case DP_TEST_PARSE_TOTAL_LENGTH_BEYOND_FRAME:
        dp_test_put_be16(ipv4 + 2, DP_TEST_IPV4_MIN_LEN +
                         DP_TEST_TCP_MIN_LEN + 1);
        break;
    case DP_TEST_PARSE_TCP_TRUNCATED:
        dp_test_put_be16(ipv4 + 2, DP_TEST_IPV4_MIN_LEN +
                         DP_TEST_TCP_MIN_LEN - 1);
        *frame_len = DP_TEST_L2_LEN + DP_TEST_IPV4_MIN_LEN +
                     DP_TEST_TCP_MIN_LEN - 1;
        break;
    case DP_TEST_PARSE_INVALID_TCP_OFFSET:
        l4[12] = 0x40;
        break;
    case DP_TEST_PARSE_TCP_HEADER_BEYOND_PAYLOAD:
        l4[12] = 0x60;
        break;
    case DP_TEST_PARSE_UDP_TRUNCATED:
        dp_test_put_be16(ipv4 + 2, DP_TEST_IPV4_MIN_LEN + DP_TEST_UDP_LEN - 1);
        *frame_len = DP_TEST_L2_LEN + DP_TEST_IPV4_MIN_LEN + DP_TEST_UDP_LEN - 1;
        break;
    case DP_TEST_PARSE_INVALID_UDP_LENGTH:
        dp_test_put_be16(l4 + 4, DP_TEST_UDP_LEN - 1);
        break;
    case DP_TEST_PARSE_UDP_BEYOND_PAYLOAD:
        dp_test_put_be16(l4 + 4, DP_TEST_UDP_LEN + 1);
        break;
    case DP_TEST_PARSE_MULTI_SEGMENT:
        *nb_segs = 2;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

int
dp_test_parse_fixture(int fixture, struct dp_packet_meta *meta)
{
    uint8_t frame[DP_TEST_FRAME_CAPACITY];
    struct rte_mbuf mbuf = {0};
    uint16_t frame_len;
    uint16_t nb_segs;
    int ret;

    if (!meta)
        return -EINVAL;
    ret = dp_test_prepare_fixture(fixture, frame, &frame_len, &nb_segs);
    if (ret < 0)
        return ret;

    /* parser 只读取这些标准 mbuf 字段；fixture 不需要 EAL 或 mempool。 */
    mbuf.buf_addr = frame;
    mbuf.data_off = 0;
    mbuf.data_len = frame_len;
    mbuf.pkt_len = frame_len;
    mbuf.nb_segs = nb_segs;
    return dp_parse_packet(&mbuf, meta);
}

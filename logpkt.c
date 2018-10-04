/*-
 * SSLsplit - transparent SSL/TLS interception
 * https://www.roe.ch/SSLsplit
 *
 * Copyright (c) 2009-2018, Daniel Roethlisberger <daniel@roe.ch>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "logpkt.h"

#include "sys.h"
#include "log.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <pcap.h>
#include <errno.h>

#if defined(__OpenBSD__) && !defined(ETHERTYPE_IPV6)
#include <net/ethertypes.h>
#endif /* __OpenBSD__ && !ETHERTYPE_IPV6 */

#define MSS_VAL 1420

typedef struct __attribute__((packed)) {
	uint32_t magic_number;  /* magic number */
	uint16_t version_major; /* major version number */
	uint16_t version_minor; /* minor version number */
	uint32_t thiszone;      /* GMT to local correction */
	uint32_t sigfigs;       /* accuracy of timestamps */
	uint32_t snaplen;       /* max length of captured packets, in octets */
	uint32_t network;       /* data link type */
} pcap_file_hdr_t;

typedef struct __attribute__((packed)) {
	uint32_t ts_sec;        /* timestamp seconds */
	uint32_t ts_usec;       /* timestamp microseconds */
	uint32_t incl_len;      /* number of octets of packet saved in file */
	uint32_t orig_len;      /* actual length of packet */
} pcap_rec_hdr_t;

libnet_t *libnet_pcap = NULL; /* XXX */
libnet_t *libnet_mirror = NULL; /* XXX */

#define PCAP_MAGIC 0xa1b2c3d4

static int
logpkt_write_global_pcap_hdr(int fd)
{
	pcap_file_hdr_t hdr;

	memset(&hdr, 0x0, sizeof(hdr));
	hdr.magic_number = PCAP_MAGIC;
	hdr.version_major = 2;
	hdr.version_minor = 4;
	hdr.snaplen = 1500;
	hdr.network = 1;
	return write(fd, &hdr, sizeof(hdr)) != sizeof(hdr) ? -1 : 0;
}

/*
 * Called on a file descriptor open for reading and writing.
 * If the fd points to an empty file, a pcap header is added and 0 is returned.
 * If the fd points to a file with PCAP magic bytes, the file position is moved
 * to the end of the file and 0 is returned.
 * If the fd points to a file without PCAP magic bytes, the file is truncated
 * to zero bytes and a new PCAP header is written.
 * On a return value of 0, the caller can continue to write PCAP records to the
 * file descriptor.  On error, -1 is returned and the file descriptor is in an
 * undefined but still open state.
 */
int
logpkt_pcap_open_fd(int fd) {
	pcap_file_hdr_t hdr;
	off_t sz;
	ssize_t n;

	sz = lseek(fd, 0, SEEK_END);
	if (sz == -1)
		return -1;

	if (sz > 0) {
		if (lseek(fd, 0, SEEK_SET) == -1)
			return -1;
		n = read(fd, &hdr, sizeof(pcap_file_hdr_t));
		if (n != sizeof(pcap_file_hdr_t))
			return -1;
		if (hdr.magic_number == PCAP_MAGIC)
			return lseek(fd, 0, SEEK_END) == -1 ? -1 : 0;
		if (lseek(fd, 0, SEEK_SET) == -1)
			return -1;
		if (ftruncate(fd, 0) == -1)
			return -1;
	}

	return logpkt_write_global_pcap_hdr(fd);
}

/*
 * Returns 1 if addr is equal to ip6 error addr, 0 otherwise.
 */
static int
logpkt_ip6addr_is_error(struct libnet_in6_addr *addr)
{
	if (memcmp(&addr->__u6_addr, &in6addr_error.__u6_addr, 16) == 0)
		return 1;
	return 0;
}

static int
logpkt_str2ip46addr(libnet_t *libnet, char *addr, int af,
                    unsigned int *ip4addr, struct libnet_in6_addr *ip6addr)
{
	if (af == AF_INET) {
		*ip4addr = inet_addr(addr);
		if (*ip4addr == 0) {
			log_err_printf("Error converting IPv4 address: %s\n",
			               addr);
			goto out;
		}
	} else {
		*ip6addr = libnet_name2addr6(libnet, addr, LIBNET_DONT_RESOLVE);
		if (logpkt_ip6addr_is_error(ip6addr)) {
			log_err_printf("Error converting IPv6 address: %s\n",
			               addr);
			goto out;
		}
	}
	return 0;
out:
	return -1;
}

int
logpkt_set_packet_fields(libnet_t *libnet, pcap_packet_t *ctx,
                         char *src_addr, char *src_port,
                         char *dst_addr, char *dst_port)
{
	ctx->af = sys_get_af(src_addr);
	if (ctx->af == AF_UNSPEC) {
		log_err_printf("Unspec address family: %s\n", src_addr);
		goto out;
	}
	if (sys_get_af(dst_addr) != ctx->af) {
		log_err_printf("Src and dst address families do not match"
		               ": %s, %s\n", src_addr, dst_addr);
		goto out;
	}

	if (logpkt_str2ip46addr(libnet, src_addr, ctx->af,
	                        &ctx->src_ip, &ctx->src_ip6) == -1) {
		goto out;
	}
	ctx->src_port = atoi(src_port);

	if (logpkt_str2ip46addr(libnet, dst_addr, ctx->af,
	                        &ctx->dst_ip, &ctx->dst_ip6) == -1) {
		goto out;
	}
	ctx->dst_port = atoi(dst_port);

	ctx->seq = 0;
	ctx->ack = 0;
	return 0;
out:
	return -1;
}

static int
logpkt_write_pcap_record(int fd)
{
	uint32_t len;
	uint8_t *packet = NULL;
	pcap_rec_hdr_t packet_record_hdr;
	struct timeval tv;
	int rv = -1;

	if (libnet_pblock_coalesce(libnet_pcap, &packet, &len) == -1) {
		log_err_printf("Error in libnet_pblock_coalesce(): %s",
		               libnet_geterror(libnet_pcap));
		goto out;
	}

	gettimeofday(&tv, NULL);
	packet_record_hdr.ts_sec = tv.tv_sec;
	packet_record_hdr.ts_usec = tv.tv_usec;
	packet_record_hdr.orig_len = packet_record_hdr.incl_len = len;

	if (write(fd, &packet_record_hdr, sizeof(packet_record_hdr))
	    == sizeof(packet_record_hdr)) {
		if (write(fd, packet, len) != (int)len) {
			log_err_printf("Error writing pcap record packet"
			               ": %s\n", strerror(errno));
			goto out2;
		}
	} else {
		log_err_printf("Error writing pcap record hdr: %s\n",
		               strerror(errno));
		goto out2;
	}

	rv = 0;
out2:
	/* XXX double-check this */
	if (libnet_pcap->aligner > 0) {
		// Don't forget to free aligned bytes
		packet = packet - libnet_pcap->aligner;
	}
	free(packet);
out:
	return rv;
}

int
logpkt_write_payload(libnet_t *libnet, int fd,
                     pcap_packet_t *from, pcap_packet_t *to, char flags,
                     const uint8_t *payload, size_t payloadlen)
{
	int sendsize = 0;

	while (payloadlen > 0) {
		payload += sendsize;
		sendsize = payloadlen > MSS_VAL ? MSS_VAL : payloadlen;

		if (logpkt_write_packet(libnet, fd, from, flags, payload,
		                        sendsize) == -1) {
			log_err_printf("Warning: Failed to write to pcap log"
			               ": %s\n", strerror(errno));
			return -1;
		}

		to->ack += sendsize;
		payloadlen -= sendsize;
	}

	if (logpkt_write_packet(libnet, fd, to, TH_ACK, NULL, 0) == -1) {
		log_err_printf("Warning: Failed to write to pcap log: %s\n",
		               strerror(errno));
		return -1;
	}
	return 0;
}

static int
logpkt_build_packet(libnet_t *libnet, pcap_packet_t *ctx, char flags,
                    const uint8_t *payload, size_t payloadlen)
{
	libnet_ptag_t ptag;

	if (flags & TH_SYN) {
		ctx->seq = libnet_get_prand(LIBNET_PRu32);
	}

	ptag = libnet_build_tcp(
			ctx->src_port, /* source port */
			ctx->dst_port, /* destination port */
			ctx->seq, /* sequence number */
			ctx->ack, /* acknowledgement num */
			flags, /* control flags */
			32767, /* window size */
			0, /* checksum */
			0, /* urgent pointer */
			LIBNET_TCP_H + payloadlen, /* TCP packet size */
			// payload type differs in different libnet versions
			(uint8_t *)payload, /* payload */
			payloadlen, /* payload size */
			libnet, /* libnet handle */
			0); /* libnet id */
	if (ptag == -1) {
		log_err_printf("Error building tcp header: %s",
		               libnet_geterror(libnet));
		goto out;
	}

	if (ctx->af == AF_INET) {
		ptag = libnet_build_ipv4(
				LIBNET_IPV4_H + LIBNET_TCP_H + payloadlen, /* length */
				0, /* TOS */
				(uint16_t)libnet_get_prand(LIBNET_PRu16), /* IP ID */
				0x4000, /* IP Frag */
				64, /* TTL */
				IPPROTO_TCP, /* protocol */
				0, /* checksum */
				ctx->src_ip, /* source IP */
				ctx->dst_ip, /* destination IP */
				NULL, /* payload */
				0, /* payload size */
				libnet, /* libnet handle */
				0); /* libnet id */
	} else {
		// TODO: Check values of tc, fl, nh, and hl
		ptag = libnet_build_ipv6(
				0, /* traffic class */
				0, /* flow label */
				LIBNET_IPV6_H + LIBNET_TCP_H + payloadlen, /* total length of the IP packet */
				IPPROTO_TCP, /* next header */
				255, /* hop limit */
				ctx->src_ip6, /* source IPv6 address */
				ctx->dst_ip6, /* destination IPv6 address */
				NULL, /* optional payload or NULL */
				0, /* payload length or 0 */
				libnet, /* pointer to a libnet context */
				0); /* protocol tag to modify an existing header, 0 to build a new one */
	}
	if (ptag == -1) {
		log_err_printf("Error building ip header: %s", libnet_geterror(libnet));
		goto out;
	}

	ptag = libnet_build_ethernet(
			ctx->dst_ether, /* ethernet destination */
			ctx->src_ether, /* ethernet source */
			ctx->af == AF_INET ? ETHERTYPE_IP : ETHERTYPE_IPV6, /* protocol type */
			NULL, /* payload */
			0, /* payload size */
			libnet, /* libnet handle */
			0); /* libnet id */
	if (ptag == -1) {
		log_err_printf("Error building ethernet header: %s", libnet_geterror(libnet));
		goto out;
	}

	ctx->seq += payloadlen;
out:
	return ptag;
}

int
logpkt_write_packet(libnet_t *libnet, int fd, pcap_packet_t *ctx, char flags,
                    const uint8_t *payload, size_t payloadlen)
{
	int rv;

	if (logpkt_build_packet(libnet, ctx, flags,
	                        payload, payloadlen) == -1) {
		log_err_printf("Error building packet\n");
		return -1;
	}

	if (libnet == libnet_pcap) {
		rv = logpkt_write_pcap_record(fd);
	} else {
		rv = libnet_write(libnet);
	}
	if (rv == -1) {
		log_err_printf("Error writing packet: %s",
		               libnet_geterror(libnet));
	}

	libnet_clear_packet(libnet);
	return rv;
}

typedef struct {
	uint32_t ip;
	int result;
	uint8_t ether[ETHER_ADDR_LEN];
} logpkt_recv_arp_reply_ctx_t;

static void
logpkt_recv_arp_reply(uint8_t *user,
                      UNUSED const struct pcap_pkthdr *h,
                      const uint8_t *packet)
{
	logpkt_recv_arp_reply_ctx_t *ctx = (logpkt_recv_arp_reply_ctx_t*)user;
	struct libnet_802_3_hdr *heth = (void*)packet;
	struct libnet_arp_hdr *harp = (void*)((char*)heth + LIBNET_ETH_H);

	/* skip if wrong protocol */
	if (htons(harp->ar_op) != ARPOP_REPLY)
		return;
	if (htons(harp->ar_pro) != ETHERTYPE_IP)
		return;
	if (htons(harp->ar_hrd) != ARPHRD_ETHER)
		return;

	/* skip if wrong target IP address */
	if (!!memcmp(&ctx->ip, (char*)harp + harp->ar_hln + LIBNET_ARP_H, 4))
		return;

	/* skip if source ether mismatch */
	if (!!memcmp((u_char*)harp + sizeof(struct libnet_arp_hdr),
	             heth->_802_3_shost, ETHER_ADDR_LEN))
		return;

	memcpy(ctx->ether,
	       (u_char*)harp + sizeof(struct libnet_arp_hdr),
	       ETHER_ADDR_LEN);
	ctx->result = 0;
}

/*
 * Currently, only IPv4 mirror targets are supported.
 */
int
logpkt_ether_lookup(uint8_t *src_ether, uint8_t *dst_ether,
                    const char *dst_ip_s, const char *dst_if_s)
{
	char errbuf[LIBNET_ERRBUF_SIZE > PCAP_ERRBUF_SIZE ?
	            LIBNET_ERRBUF_SIZE : PCAP_ERRBUF_SIZE];
	uint8_t broadcast_ether[ETHER_ADDR_LEN] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	uint8_t zero_ether[ETHER_ADDR_LEN] = {
		0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
	struct libnet_ether_addr *src_ether_addr;
	uint32_t src_ip;
	struct bpf_program bp;
	int count = 50;
	logpkt_recv_arp_reply_ctx_t ctx;

	ctx.ip = libnet_name2addr4(libnet_mirror, (char *)dst_ip_s,
	                           LIBNET_DONT_RESOLVE);
	if (ctx.ip == (uint32_t)-1) {
		log_err_printf("Error converting dst IP address: %s\n",
		               libnet_geterror(libnet_mirror));
		goto out2;
	}
	src_ip = libnet_get_ipaddr4(libnet_mirror);
	if (src_ip == (uint32_t)-1) {
		log_err_printf("Error getting src IP address: %s\n",
		               libnet_geterror(libnet_mirror));
		goto out2;
	}
	src_ether_addr = libnet_get_hwaddr(libnet_mirror);
	if (src_ether_addr == NULL) {
		log_err_printf("Error getting src ethernet address: %s\n",
		               libnet_geterror(libnet_mirror));
		goto out2;
	}
	memcpy(src_ether, src_ether_addr->ether_addr_octet, ETHER_ADDR_LEN);

	if (libnet_autobuild_arp(ARPOP_REQUEST,
	                         src_ether,
	                         (uint8_t*)&src_ip,
	                         zero_ether,
	                         (uint8_t*)&ctx.ip,
	                         libnet_mirror) == -1) {
		log_err_printf("Error building arp header: %s\n",
		               libnet_geterror(libnet_mirror));
		goto out2;
	}

	if (libnet_autobuild_ethernet(broadcast_ether,
	                              ETHERTYPE_ARP,
	                              libnet_mirror) == -1) {
		log_err_printf("Error building ethernet header: %s",
		               libnet_geterror(libnet_mirror));
		goto out2;
	}

	pcap_t *pcap = pcap_open_live(dst_if_s, 100, 0, 10, errbuf);
	if (pcap == NULL) {
		log_err_printf("Error in pcap_open_live(): %s\n", errbuf);
		goto out2;
	}

	if (pcap_compile(pcap, &bp, "arp", 0, -1) == -1) {
		log_err_printf("Error in pcap_compile(): %s\n",
		               pcap_geterr(pcap));
		goto out3;
	}
	if (pcap_setfilter(pcap, &bp) == -1) {
		log_err_printf("Error in pcap_setfilter(): %s\n",
		               pcap_geterr(pcap));
		goto out4;
	}

	ctx.result = -1;
	do {
		if (libnet_write(libnet_mirror) != -1) {
			/* Limit # of packets to process, so we can loop to
			 * send arp requests on busy networks. */
			if (pcap_dispatch(pcap, 1000,
			                  (pcap_handler)logpkt_recv_arp_reply,
			                  (u_char*)&ctx) < 0) {
				log_err_printf("Error in pcap_dispatch(): %s\n",
				               pcap_geterr(pcap));
				break;
			}
		} else {
			log_err_printf("Error writing arp packet: %s",
			               libnet_geterror(libnet_mirror));
			break;
		}
		sleep(1);
	} while (ctx.result == -1 && --count > 0);

	if (ctx.result == 0) {
		memcpy(dst_ether, &ctx.ether, ETHER_ADDR_LEN);
		log_dbg_printf("Mirror target is up: "
		               "%02x:%02x:%02x:%02x:%02x:%02x\n",
		               dst_ether[0], dst_ether[1], dst_ether[2],
		               dst_ether[3], dst_ether[4], dst_ether[5]);
	}

out4:
	pcap_freecode(&bp);
out3:
	pcap_close(pcap);
out2:
	return ctx.result;
}


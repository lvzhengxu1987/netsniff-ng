/*
 * netsniff-ng - the packet sniffing beast
 * Copyright 2009, 2010 Daniel Borkmann.
 * Copyright 2014 Tobias Klauser.
 * Subject to the GPL, version 2.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>

#include "xmalloc.h"
#include "die.h"
#include "ring_rx.h"
#include "built_in.h"

#ifdef HAVE_TPACKET3
static inline bool is_tpacket_v3(int sock)
{
	return get_sockopt_tpacket(sock) == TPACKET_V3;
}

static inline size_t get_ring_layout_size(struct ring *ring, bool v3)
{
	return v3 ? sizeof(ring->layout3) : sizeof(ring->layout);
}
#else
static inline bool is_tpacket_v3(int sock __maybe_unused)
{
	return false;
}

static inline size_t get_ring_layout_size(struct ring *ring, bool v3 __maybe_unused)
{
	return sizeof(ring->layout);
}
#endif /* HAVE_TPACKET3 */

void destroy_rx_ring(int sock, struct ring *ring)
{
	int ret;
	bool v3 = is_tpacket_v3(sock);

	munmap(ring->mm_space, ring->mm_len);
	ring->mm_len = 0;

	xfree(ring->frames);

	/* In general, this is freed during close(2) anyway. */
	if (v3)
		return;

	fmemset(&ring->layout, 0, sizeof(ring->layout));
	ret = setsockopt(sock, SOL_PACKET, PACKET_RX_RING, &ring->layout,
			 sizeof(ring->layout));
	if (unlikely(ret))
		panic("Cannot destroy the RX_RING: %s!\n", strerror(errno));
}

static void setup_rx_ring_layout(int sock, struct ring *ring, size_t size,
				 bool jumbo_support, bool v3)
{
	fmemset(&ring->layout, 0, sizeof(ring->layout));

	ring->layout.tp_block_size = (jumbo_support ?
				      RUNTIME_PAGE_SIZE << 4 :
				      RUNTIME_PAGE_SIZE << 2);

	ring->layout.tp_frame_size = (jumbo_support ?
				      TPACKET_ALIGNMENT << 12 :
				      TPACKET_ALIGNMENT << 7);

	ring->layout.tp_block_nr = size / ring->layout.tp_block_size;
	ring->layout.tp_frame_nr = ring->layout.tp_block_size /
				   ring->layout.tp_frame_size *
				   ring->layout.tp_block_nr;

	if (v3) {
		/* Pass out, if this will ever change and we do crap on it! */
		build_bug_on(offsetof(struct tpacket_req, tp_frame_nr) !=
			     offsetof(struct tpacket_req3, tp_frame_nr) &&
			     sizeof(struct tpacket_req) !=
			     offsetof(struct tpacket_req3, tp_retire_blk_tov));

		ring->layout3.tp_retire_blk_tov = 100; /* 0: let kernel decide */
		ring->layout3.tp_sizeof_priv = 0;
		ring->layout3.tp_feature_req_word = 0;

		set_sockopt_tpacket_v3(sock);
	} else {
		set_sockopt_tpacket_v2(sock);
	}

	ring_verify_layout(ring);
}

static void create_rx_ring(int sock, struct ring *ring, bool verbose)
{
	int ret;
	bool v3 = is_tpacket_v3(sock);
	size_t layout_size = get_ring_layout_size(ring, v3);

retry:
	ret = setsockopt(sock, SOL_PACKET, PACKET_RX_RING, &ring->raw,
			 layout_size);

	if (errno == ENOMEM && ring->layout.tp_block_nr > 1) {
		ring->layout.tp_block_nr >>= 1;
		ring->layout.tp_frame_nr = ring->layout.tp_block_size / 
					   ring->layout.tp_frame_size * 
					   ring->layout.tp_block_nr;
		goto retry;
	}
	if (ret < 0)
		panic("Cannot allocate RX_RING!\n");

	ring->mm_len = (size_t) ring->layout.tp_block_size * ring->layout.tp_block_nr;

	if (verbose) {
		if (!v3) {
			printf("RX,V2: %.2Lf MiB, %u Frames, each %u Byte allocated\n",
			       (long double) ring->mm_len / (1 << 20),
			       ring->layout.tp_frame_nr, ring->layout.tp_frame_size);
		} else {
			printf("RX,V3: %.2Lf MiB, %u Blocks, each %u Byte allocated\n",
			       (long double) ring->mm_len / (1 << 20),
			       ring->layout.tp_block_nr, ring->layout.tp_block_size);
		}
	}
}

static void alloc_rx_ring_frames(int sock, struct ring *ring)
{
	int num;
	size_t size;
	bool v3 = is_tpacket_v3(sock);

	if (v3) {
		num = ring->layout3.tp_block_nr;
		size = ring->layout3.tp_block_size;
	} else {
		num = ring->layout.tp_frame_nr;
		size = ring->layout.tp_frame_size;
	}

	alloc_ring_frames_generic(ring, num, size);
}

void ring_rx_setup(struct ring *ring, int sock, size_t size, int ifindex,
		   struct pollfd *poll, bool v3, bool jumbo_support,
		   bool verbose)
{
	fmemset(ring, 0, sizeof(*ring));
	setup_rx_ring_layout(sock, ring, size, jumbo_support, v3);
	create_rx_ring(sock, ring, verbose);
	mmap_ring_generic(sock, ring);
	alloc_rx_ring_frames(sock, ring);
	bind_ring_generic(sock, ring, ifindex, false);
	prepare_polling(sock, poll);
}

void sock_rx_net_stats(int sock, unsigned long seen)
{
	int ret;
	bool v3 = is_tpacket_v3(sock);
	union {
		struct tpacket_stats	k2;
		struct tpacket_stats_v3 k3;
	} stats;
	socklen_t slen = v3 ? sizeof(stats.k3) : sizeof(stats.k2);

	memset(&stats, 0, sizeof(stats));
	ret = getsockopt(sock, SOL_PACKET, PACKET_STATISTICS, &stats, &slen);
	if (ret > -1) {
		uint64_t packets = stats.k3.tp_packets;
		uint64_t drops = stats.k3.tp_drops;

		printf("\r%12"PRIu64"  packets incoming (%"PRIu64" unread on exit)\n",
		       v3 ? (uint64_t)seen : packets, v3 ? packets - seen : 0);
		printf("\r%12"PRIu64"  packets passed filter\n", packets - drops);
		printf("\r%12"PRIu64"  packets failed filter (out of space)\n", drops);
		if (stats.k3.tp_packets > 0)
			printf("\r%12.4lf%% packet droprate\n",
			       (1.0 * drops / packets) * 100.0);
	}
}

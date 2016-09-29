/*-
 *   BSD LICENSE
 * 
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 * 
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <string.h>
#include <sys/queue.h>
#include <stdarg.h>
#include <errno.h>
#include <getopt.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_lpm.h>

#include "main.h"

#ifndef APP_LCORE_IO_FLUSH
#define APP_LCORE_IO_FLUSH           1000000
#endif

#ifndef APP_LCORE_WORKER_FLUSH
#define APP_LCORE_WORKER_FLUSH       1000000
#endif

#ifndef APP_STATS
#define APP_STATS                    1000000
#endif

#define APP_IO_RX_DROP_ALL_PACKETS   1
#define APP_WORKER_DROP_ALL_PACKETS  0
#define APP_IO_TX_DROP_ALL_PACKETS   0

#ifndef APP_IO_RX_PREFETCH_ENABLE
#define APP_IO_RX_PREFETCH_ENABLE    1
#endif

#ifndef APP_WORKER_PREFETCH_ENABLE
#define APP_WORKER_PREFETCH_ENABLE   1
#endif

#ifndef APP_IO_TX_PREFETCH_ENABLE
#define APP_IO_TX_PREFETCH_ENABLE    1
#endif

#if APP_IO_RX_PREFETCH_ENABLE
#define APP_IO_RX_PREFETCH0(p)       rte_prefetch0(p)
#define APP_IO_RX_PREFETCH1(p)       rte_prefetch1(p)
#else
#define APP_IO_RX_PREFETCH0(p)
#define APP_IO_RX_PREFETCH1(p)
#endif

#if APP_WORKER_PREFETCH_ENABLE
#define APP_WORKER_PREFETCH0(p)      rte_prefetch0(p)
#define APP_WORKER_PREFETCH1(p)      rte_prefetch1(p)
#else
#define APP_WORKER_PREFETCH0(p)
#define APP_WORKER_PREFETCH1(p)
#endif

#if APP_IO_TX_PREFETCH_ENABLE
#define APP_IO_TX_PREFETCH0(p)       rte_prefetch0(p)
#define APP_IO_TX_PREFETCH1(p)       rte_prefetch1(p)
#else
#define APP_IO_TX_PREFETCH0(p)
#define APP_IO_TX_PREFETCH1(p)
#endif

uint8_t icmppkt []={
0x00, 0x1b, 0x21, 0xad, 0xa9, 0x9c, 0x14, 0xdd, 0xa9, 0xd2, 0xef, 0x57, 0x08, 0x00, 0x45, 0x00,
0x00, 0x54, 0x51, 0x36, 0x40, 0x00, 0x40, 0x01, 0x6b, 0xee, 0x96, 0xf4, 0x3a, 0x72, 0xd8, 0x3a,
0xd3, 0xe3, 0x08, 0x00, 0x00, 0x00, 0x66, 0x02, 0x00, 0x1a, 0x67, 0x72, 0x97, 0x57, 0x00, 0x00,
0x00, 0x00, 0xe4, 0x64, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
0x36, 0x37
};

int doChecksum = 0;
int autoIncNum = 0;
int icmppktlen = sizeof(icmppkt);

//#define QUEUE_STATS
static inline void
app_lcore_io_rx(
	struct app_lcore_params_io *lp,
	uint32_t n_workers,
	uint32_t bsz_rd,
	uint32_t bsz_wr,
	uint8_t pos_lb)
{
	uint32_t i;

	(void)n_workers;
	(void)bsz_wr;
	(void)pos_lb;

	static uint32_t counter;

	for (i = 0; i < lp->rx.n_nic_queues; i ++) {
		uint8_t port = lp->rx.nic_queues[i].port;
		uint8_t queue = lp->rx.nic_queues[i].queue;
		uint32_t n_mbufs, j;

		n_mbufs = rte_eth_rx_burst(
			port,
			queue,
			lp->rx.mbuf_in.array,
			(uint16_t) bsz_rd);

		if (unlikely(n_mbufs == 0)) {
			continue;
		}

		if(++counter>APP_STATS){
			printf("Latency %lu ns\n",hptl_get()-(*(hptl_t*)(rte_ctrlmbuf_data(lp->rx.mbuf_in.array[n_mbufs-1])+rte_ctrlmbuf_len(lp->rx.mbuf_in.array[n_mbufs-1])-8)));
			counter =0;
		}

#if APP_STATS
		lp->rx.nic_queues_iters[i] ++;
		lp->rx.nic_queues_count[i] += n_mbufs;
		if (unlikely(lp->rx.nic_queues_iters[i] == APP_STATS*10)) {
			struct rte_eth_stats stats;
			struct timeval start_ewr, end_ewr;

			rte_eth_stats_get(port, &stats);
			gettimeofday(&lp->rx.end_ewr, NULL);

			start_ewr = lp->rx.start_ewr; end_ewr = lp->rx.end_ewr;

			if(lp->rx.record)
			{
				fprintf(lp->rx.record,"%lu\t%lf\t%.1lf\t%u\n",
				start_ewr.tv_sec,
				(((stats.ibytes)+stats.ipackets*(/*4crc+8prelud+12ifg*/(8+12)))/(((end_ewr.tv_sec * 1000000. + end_ewr.tv_usec) - (start_ewr.tv_sec * 1000000. + start_ewr.tv_usec))/1000000.))/(1000*1000*1000./8.),
				(double)stats.ipackets/((((double)end_ewr.tv_sec * (double)1000000. + (double)end_ewr.tv_usec) - ((double)start_ewr.tv_sec * (double)1000000. + (double)start_ewr.tv_usec)) /(double)1000000.),
				(uint32_t) stats.ierrors
				);
				fflush(lp->rx.record);
			}
			else
			{
#ifdef QUEUE_STATS
			if(queue==0)
			{
#endif
			printf("NIC port %u: drop ratio = %.2f (%u/%u) speed: %lf Gbps (%.1lf pkts/s)\n",
				(unsigned) port,
				(double) stats.ierrors / (double) (stats.ierrors + stats.ipackets),
				(uint32_t) stats.ipackets, (uint32_t) stats.ierrors,
				(((stats.ibytes)+stats.ipackets*(/*4crc+8prelud+12ifg*/(8+12)))/(((end_ewr.tv_sec * 1000000. + end_ewr.tv_usec) - (start_ewr.tv_sec * 1000000. + start_ewr.tv_usec))/1000000.))/(1000*1000*1000./8.),
				stats.ipackets/(((end_ewr.tv_sec * 1000000. + end_ewr.tv_usec) - (start_ewr.tv_sec * 1000000. + start_ewr.tv_usec)) /1000000.)
				);
#ifdef QUEUE_STATS
			}
			printf("NIC port %u:%u: drop ratio = %.2f (%u/%u) speed %.1lf pkts/s\n",
				(unsigned) port, queue,
				(double) stats.ierrors / (double) (stats.ierrors + lp->rx.nic_queues_count[i]),
				(uint32_t) lp->rx.nic_queues_count[i], (uint32_t) stats.ierrors,
				lp->rx.nic_queues_count[i]/(((end_ewr.tv_sec * 1000000. + end_ewr.tv_usec) - (start_ewr.tv_sec * 1000000. + start_ewr.tv_usec)) /1000000.)
				);
#endif
			}
			lp->rx.nic_queues_iters[i] = 0;
			lp->rx.nic_queues_count[i] = 0;

#ifdef QUEUE_STATS
                       	if(queue==0)
#endif
			rte_eth_stats_reset (port);

#ifdef QUEUE_STATS
                  	if(queue==0)
#endif
			lp->rx.start_ewr = end_ewr; // Updating start
		}
#endif

#if APP_IO_RX_DROP_ALL_PACKETS
		for (j = 0; j < n_mbufs; j ++) {
			struct rte_mbuf *pkt = lp->rx.mbuf_in.array[j];
			rte_pktmbuf_free(pkt);
		}

		continue;
#endif

	}
}

static inline void
app_lcore_io_tx(
	struct app_lcore_params_io *lp,
	uint32_t n_workers,
	uint32_t bsz_rd,
	uint32_t bsz_wr)
{
	uint32_t worker;
	
	for (worker = 0; worker < n_workers; worker ++) {
		uint32_t i;

		for (i = 0; i < lp->tx.n_nic_ports; i ++) {
			uint8_t port = lp->tx.nic_ports[i];
			//struct rte_ring *ring = lp->tx.rings[port][worker];
			uint32_t n_mbufs, n_pkts;
			//int ret;

			//UNUSED -> uncoment :)
			(void)bsz_rd;
			(void)bsz_wr;

			//n_mbufs = lp->tx.mbuf_out[port].n_mbufs;
			/*ret = rte_ring_sc_dequeue_bulk(
				ring,
				(void **) &lp->tx.mbuf_out[port].array[n_mbufs],
				bsz_rd);

			if (unlikely(ret == -ENOENT)) {
				continue;
			}

			n_mbufs += bsz_rd;*/

			n_mbufs = 1;

			struct rte_mbuf * tmpbuf = rte_ctrlmbuf_alloc(app.pools[0]) ;
			if(!tmpbuf){
				continue;
			}

			tmpbuf->pkt_len = icmppktlen;
			tmpbuf->data_len = icmppktlen;
			tmpbuf->port = port;

			int icmpStart = 6+6+2+5*4;
			
			if(autoIncNum){
				(*((uint16_t*)(icmppkt+icmpStart+2+2+2)))++;
			}

			memcpy(rte_ctrlmbuf_data(tmpbuf),icmppkt,icmppktlen-8);
			*((hptl_t*)(rte_ctrlmbuf_data(tmpbuf)+icmppktlen-8)) = hptl_get();

			if(doChecksum){
				uint16_t cksum;
				cksum = rte_raw_cksum (rte_ctrlmbuf_data(tmpbuf)+icmpStart, icmppktlen-icmpStart);
				*((uint16_t*)(rte_ctrlmbuf_data(tmpbuf)+icmpStart+2)) = ((cksum == 0xffff) ? cksum : ~cksum);
			}

			/*if (unlikely(n_mbufs < bsz_wr)) {
				lp->tx.mbuf_out[port].n_mbufs = n_mbufs;
				continue;
			}*/

			n_pkts = rte_eth_tx_burst(
				port,
				0,
				&tmpbuf,
				n_mbufs);
			
			if (unlikely(n_pkts < n_mbufs)){
				rte_ctrlmbuf_free(tmpbuf);
			}
			/*if (unlikely(n_pkts < n_mbufs)) {
				uint32_t k;
				for (k = n_pkts; k < n_mbufs; k ++) {
					struct rte_mbuf *pkt_to_free = lp->tx.mbuf_out[port].array[k];
					rte_pktmbuf_free(pkt_to_free);
				}
			}*/
			lp->tx.mbuf_out[port].n_mbufs = 0;
			lp->tx.mbuf_out_flush[port] = 0;
		}
	}
}

static void
app_lcore_main_loop_io(void)
{
	uint32_t lcore = rte_lcore_id();
	struct app_lcore_params_io *lp = &app.lcore_params[lcore].io;
	uint32_t n_workers = app_get_lcores_worker();
	uint64_t i = 0;

	uint32_t bsz_rx_rd = app.burst_size_io_rx_read;
	uint32_t bsz_rx_wr = app.burst_size_io_rx_write;

	uint8_t pos_lb = app.pos_lb;

	for ( ; ; ) {
		if (likely(lp->rx.n_nic_queues > 0)) {
			app_lcore_io_rx(lp, n_workers, bsz_rx_rd, bsz_rx_wr, pos_lb); 
		}

		if (likely(lp->tx.n_nic_ports > 0)) {
			app_lcore_io_tx(lp, 1, app.burst_size_io_tx_read, app.burst_size_io_tx_write); 
		}

		i ++;
	}
}

int
app_lcore_main_loop(__attribute__((unused)) void *arg)
{
	struct app_lcore_params *lp;
	unsigned lcore;

	lcore = rte_lcore_id();
	lp = &app.lcore_params[lcore];

	if (lp->type == e_APP_LCORE_IO) {
		printf("Logical core %u (I/O) main loop.\n", lcore);
		app_lcore_main_loop_io();
	}
	
	return 0;
}

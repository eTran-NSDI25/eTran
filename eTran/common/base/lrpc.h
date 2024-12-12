/*
 * lrpc.h - shared memory communication channels
 *
 * This design is inspired by Barrelfish, which in turn was based on Brian
 * Bershad's earlier LRPC work. The goal here is to minimize cache misses to
 * the maximum extent possible.
 * 
 * The message size is fixed to 64 bytes, which is the size of a cache line.
 */

#pragma once

#include <stdint.h>
#include <string.h>

#include <base/stddef.h>
#include <base/compiler.h>
#include <base/atomic.h>

const unsigned int LRPC_CHANNEL_SIZE = 2048;

struct head_wb {
    union {
        uint32_t recv_head_wb;
        char __pad[64];
    };
};

typedef struct __attribute__((packed))
{
    uint64_t cmd;
    union {
        uint8_t data[56];
    };
} lrpc_msg;
static_assert(sizeof(lrpc_msg) == 64, "Size of lrpc_msg is not 64 bytes");

#define LRPC_DONE_PARITY (1UL << 63)
#define LRPC_CMD_MASK (~LRPC_DONE_PARITY)

int __lrpc_send(struct lrpc_chan_out *chan, lrpc_msg *msg);
int lrpc_init_out(struct lrpc_chan_out *chan, lrpc_msg *tbl,
						 unsigned int size, uint32_t *recv_head_wb);
int lrpc_init_in(struct lrpc_chan_in *chan, lrpc_msg *tbl,
						unsigned int size, uint32_t *recv_head_wb);

/*
 * Egress Channel Support
 */
struct lrpc_chan_out
{
    lrpc_msg *tbl;
    uint32_t *recv_head_wb;
    uint32_t send_head;
    uint32_t send_tail;
    uint32_t size;
    uint32_t pad;
};

/**
 * lrpc_send - sends a message on the channel
 * @chan: the egress channel
 * @cmd: the command to send
 * @payload: the data payload
 *
 */
static inline int lrpc_send(struct lrpc_chan_out *chan, lrpc_msg *msg)
{
    lrpc_msg *dst;
    uint64_t cmd = msg->cmd;

    assert(!(cmd & LRPC_DONE_PARITY));

    if (unlikely(chan->send_head - chan->send_tail >= chan->size))
        return __lrpc_send(chan, msg);

    dst = &chan->tbl[chan->send_head & (chan->size - 1)];
    cmd |= (chan->send_head++ & chan->size) ? 0 : LRPC_DONE_PARITY;

    memcpy(dst->data, msg->data, sizeof(msg->data));

    store_release(&dst->cmd, cmd);
    return 0;
}

/*
 * Ingress Channel Support
 */

struct lrpc_chan_in
{
    lrpc_msg *tbl;
    uint32_t *recv_head_wb;
    uint32_t recv_head;
    uint32_t size;
};

static inline int lrpc_recv(struct lrpc_chan_in *chan, lrpc_msg *msg)
{
	lrpc_msg *m = &chan->tbl[chan->recv_head & (chan->size - 1)];
	uint64_t parity = (chan->recv_head & chan->size) ? 0 : LRPC_DONE_PARITY;
	uint64_t cmd;

	cmd = load_acquire(&m->cmd);
	if ((cmd & LRPC_DONE_PARITY) != parity)
		return -1;
	chan->recv_head++;

	msg->cmd = cmd & LRPC_CMD_MASK;
	
    memcpy(msg->data, m->data, sizeof(m->data));

	store_release(chan->recv_head_wb, chan->recv_head);
	return 0;
}

/**
 * lrpc_empty - returns true if the channel has no available messages
 * @chan: the ingress channel
 */
static inline bool lrpc_empty(struct lrpc_chan_in *chan)
{
	lrpc_msg *m = &chan->tbl[chan->recv_head & (chan->size - 1)];
	uint64_t parity = (chan->recv_head & chan->size) ? 0 : LRPC_DONE_PARITY;
	return (ACCESS_ONCE(m->cmd) & LRPC_DONE_PARITY) != parity;
}
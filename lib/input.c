/* NodyOS - single-producer / single-consumer input rings. */
#include "lib/input.h"

#define QN 256

static MouseEv mq[QN];
static volatile u32 mq_head, mq_tail;
static KeyEv kq[QN];
static volatile u32 kq_head, kq_tail;

static volatile u8 cur_mods;

void input_set_mods(u8 mods) { cur_mods = mods; }
u8   input_mods(void) { return cur_mods; }

void input_push_mouse(const MouseEv *e)
{
    u32 next = (mq_head + 1) % QN;
    if (next == mq_tail) return;        /* full: drop */
    mq[mq_head] = *e;
    mq_head = next;
}

bool input_pop_mouse(MouseEv *e)
{
    if (mq_tail == mq_head) return false;
    *e = mq[mq_tail];
    mq_tail = (mq_tail + 1) % QN;
    return true;
}

void input_push_key(const KeyEv *e)
{
    u32 next = (kq_head + 1) % QN;
    if (next == kq_tail) return;
    kq[kq_head] = *e;
    kq_head = next;
}

bool input_pop_key(KeyEv *e)
{
    if (kq_tail == kq_head) return false;
    *e = kq[kq_tail];
    kq_tail = (kq_tail + 1) % QN;
    return true;
}

#include "ringbuf.h"

#include <stdlib.h>
#include <string.h>

// Initializes a ring buffer with the given capacity.
bool RingBuf_Init(RingBufF *rb, uint32_t capacity)
{
    memset(rb, 0, sizeof(*rb));
    if (capacity == 0) {
        return false;
    }
    rb->data = (float *)calloc(capacity, sizeof(float));
    if (!rb->data) {
        return false;
    }
    rb->cap = capacity;
    rb->count = 0;
    rb->head = 0;
    return true;
}
// Shuts down the ring buffer and frees resources.
void RingBuf_Shutdown(RingBufF *rb)
{
    free(rb->data);
    memset(rb, 0, sizeof(*rb));
}

void RingBuf_Push(RingBufF *rb, float v)
{
    if (!rb || !rb->data || rb->cap == 0) {
        return;
    }
    rb->data[rb->head] = v;
    rb->head = (rb->head + 1) % rb->cap;
    if (rb->count < rb->cap) {
        rb->count++;
    }
}
// Helper to get the index of the oldest element.
static uint32_t oldest_index(const RingBufF *rb)
{
    // head points to next write; oldest is head when full, else 0
    if (rb->count < rb->cap) {
        return 0;
    }
    return rb->head;
}

float RingBuf_GetOldest(const RingBufF *rb, uint32_t i)
{
    if (!rb || !rb->data || rb->count == 0) {
        return 0.0f;
    }
    if (i >= rb->count) {
        i = rb->count - 1;
    }
    const uint32_t start = oldest_index(rb);
    const uint32_t idx = (start + i) % rb->cap;
    return rb->data[idx];
}

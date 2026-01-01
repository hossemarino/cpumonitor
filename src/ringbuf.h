#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct RingBufF {
    float *data;
    uint32_t cap;
    uint32_t count;
    uint32_t head;
} RingBufF;

bool RingBuf_Init(RingBufF *rb, uint32_t capacity);
void RingBuf_Shutdown(RingBufF *rb);
void RingBuf_Push(RingBufF *rb, float v);

// Gets i-th oldest element, where i=0 is oldest.
float RingBuf_GetOldest(const RingBufF *rb, uint32_t i);

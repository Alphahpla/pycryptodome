/* ===================================================================
 *
 * Copyright (c) 2018, Helder Eijs <helderijs@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * ===================================================================
 */

#include "common.h"

FAKE_INIT(poly1305)

typedef struct mac_state_t {
    uint32_t r[4], rr[4];   /** key **/
    uint8_t s[16];          /** key **/
    uint32_t h[5];          /** state **/

    uint8_t buffer[16];     /** temp input **/
    unsigned buffer_used;
} mac_state;

/*
 * Load 16 bytes as the secret r, which is the value we evaluate the polynomial
 * with, modulo 2^130-5.
 *
 * The secret gets encoded into four 32-bit words (r[]), after appropriate clamping
 * (reset) is applied to 18 of its bits.
 *
 * Additionaly, reduce modulo 2^130-5 the value 2^130*r into rr[], which we can
 * reuse several times later during each multiplication.
 */
STATIC void poly1305_load_r(uint32_t r[4], uint32_t rr[4], const uint8_t secret[16])
{
    unsigned i;
    uint32_t mask;

    for (i=0; i<4; i++) {
        /**
         * The 4 most significant bits in a word are reset.
         * The 2 least significant bits in a word are reset, except for r[0]
         */
        mask = (i==0) ? 0x0FFFFFFFU : 0x0FFFFFFCU;
        r[i] = LOAD_U32_LITTLE(secret+i*4) & mask;
        rr[i] = (r[i] >> 2)*5;
    }
}

/*
 * Convert a piece of the message (16 bytes, unless it is the last piece),
 * into an integer encoded into five 32-bit words.
 */
STATIC void poly1305_load_m(uint32_t m[5], const uint8_t data[], size_t len)
{
    uint8_t copy[sizeof(uint32_t)*5];

    assert(len<=16);

    memset(copy, 0, sizeof(copy));
    memcpy(copy, data, len);
    copy[len] = 1;  /** 2^128 or 2^{8*(l mod 16)} **/

    m[0] = LOAD_U32_LITTLE(copy);
    m[1] = LOAD_U32_LITTLE(copy+4);
    m[2] = LOAD_U32_LITTLE(copy+8);
    m[3] = LOAD_U32_LITTLE(copy+12);
    m[4] = LOAD_U32_LITTLE(copy+16);
}

/**
 * Multiply by the secret r, "almost" modulo 2^130-5.
 *
 * r[] and rr[] must have been set by poly1305_load_r().
 *
 * The result is placed into h[] and it is guaranteed to
 * be smaller than 2^131 (not 2^130-5).
 */
STATIC void poly1305_multiply(uint32_t h[5], const uint32_t r[4], const uint32_t rr[4])
{
    uint64_t a0, a1, a2, a3;
    uint64_t aa0, aa1, aa2, aa3;
    uint64_t x0, x1, x2, x3, x4;
    uint64_t carry;

    /*
     * Boundaries
     * - h[0..4]  <   2^32
     * - r[0..3]  <   2^28 < 5*2^26
     * - rr[0..3] < 5*2^26
     */

    a0 = r[0];
    a1 = r[1];
    a2 = r[2];
    a3 = r[3];
    aa0 = rr[0];
    aa1 = rr[1];
    aa2 = rr[2];
    aa3 = rr[3];

    /**
     * Schoolbook multiplication between h[] and  r[], with the caveat that
     * the components exceeding 2^130 are folded back with a right shift and
     * a multiplication by 5 (already precomputed in rr[]).
     *
     * Each sum is guaranteed to be smaller than 2^63 (x0 being the worst case).
     */
    x0 = a0*h[0] + aa0*h[4] + aa1*h[3] + aa2*h[2] + aa3*h[1];
    x1 = a0*h[1] +  a1*h[0] + aa1*h[4] + aa2*h[3] + aa3*h[2];
    x2 = a0*h[2] +  a1*h[1] +  a2*h[0] + aa2*h[4] + aa3*h[3];
    x3 = a0*h[3] +  a1*h[2] +  a2*h[1] +  a3*h[0] + aa3*h[4];
    x4 = (a0 & 3)*h[4]; /** < 2^34 **/

    /** Clear upper half of x3 **/
    x4 += x3 >> 32;
    x3 &= UINT32_MAX;

    /** Clear the 62 most significant bits of x4 and
     *  create carry for x0 **/
    carry = (x4 >> 2)*5;    /** < 2^35 **/
    x4 &= 3;

    /** Reduce x0 to 32 bits and store into h0 **/
    x0 += carry;
    h[0] = x0 & UINT32_MAX;
    carry = x0 >> 32;
    
    /** Reduce x1 to 32 bits and store into h1 **/
    x1 += carry;
    h[1] = x1 & UINT32_MAX;
    carry = x1 >> 32;

    /** Reduce x2 to 32 bits and store into h2 **/
    x2 += carry;
    h[2] = x2 & UINT32_MAX;
    carry = x2 >> 32;
    
    /** Reduce x3 to 32 bits and store into h3 **/
    x3 += carry;
    h[3] = x3 & UINT32_MAX;
    carry = x3 >> 32;   /** < 1 **/
    
    /** Reduce x4 to 32 bits and store into h4 **/
    x4 += carry;    /** < 2^3 **/
    assert(x4 < 8);
    h[4] = x4;
}

/*
 * Reduce the value h[] modulo 2^130-5.
 *
 * h[] must be smaller than 2^131.
 */
STATIC void poly1305_reduce(uint32_t h[5])
{
    unsigned i;

    assert(h[4]<8);

    for (i=0; i<2; i++) {
        uint32_t mask, carry;
        uint32_t g[5];

        /** Compute h+(-p) by adding and removing 2^130 **/
        g[0] = h[0] + 5;     carry = g[0] < h[0];
        g[1] = h[1] + carry; carry = g[1] < h[1];
        g[2] = h[2] + carry; carry = g[2] < h[2];
        g[3] = h[3] + carry; carry = g[3] < h[3];
        g[4] = h[4] + carry - 4;

        mask = (g[4] >> 31) - 1U;    /** All 1s if g[] is a valid reduction **/
        h[0] = (h[0] & ~mask) ^ (g[0] & mask);
        h[1] = (h[1] & ~mask) ^ (g[1] & mask);
        h[2] = (h[2] & ~mask) ^ (g[2] & mask);
        h[3] = (h[3] & ~mask) ^ (g[3] & mask);
        h[4] = (h[4] & ~mask) ^ (g[4] & mask);
    }
}

STATIC void poly1305_accumulate(uint32_t h[5], const uint32_t m[5])
{
#if 0
    // 128-bit type exist and little-endian
    uint32_t carry;
    __uint128_t a, b, c;

    memcpy(&a, h, 16);
    memcpy(&b, m, 16);
    c = a + b; carry = c < a;
    memcpy(h, &c, 16);
    h[4] += m[4] + carry;
#else
    uint8_t carry;
    uint64_t tmp;
    
    h[0] += m[0];
    carry = h[0] < m[0];
    
    tmp = (uint64_t)h[1] + m[1] + carry;
    h[1] = (uint32_t) tmp;
    carry = (tmp >> 32) & 1;

    tmp = (uint64_t)h[2] + m[2] + carry;
    h[2] = (uint32_t) tmp;
    carry = (tmp >> 32) & 1;

    tmp = (uint64_t)h[3] + m[3] + carry;
    h[3] = (uint32_t) tmp;
    carry = (tmp >> 32) & 1;

    tmp = (uint64_t)h[4] + m[4] + carry;
    h[4] = (uint32_t) tmp;
    
    assert((tmp >> 32) == 0);
#endif
}

static void poly1305_process(uint32_t h[5], uint32_t r[4], uint32_t rr[4], uint8_t msg[], size_t len)
{
    uint32_t m[5];

    if (len == 0)
        return;

    poly1305_load_m(m, msg, len);
    poly1305_accumulate(h, m);
    poly1305_multiply(h, r, rr);
}

/*
 * Load 16 bytes as the secret s, which is the fixed term in the polynomial
 * modulo 2^130-5.
 *
 * s[] is expected to be the result of an encryption done with AES or ChaCha20.
 */
static void poly1305_finalize(uint32_t h[5], const uint8_t s[16])
{
    uint32_t m[5];

    poly1305_load_m(m, s, 16);
    poly1305_accumulate(h, m);
    poly1305_reduce(h);
}

/* --------------------------------------------------------- */

EXPORT_SYM int poly1305_init(mac_state **pState,
                             const uint8_t *key,    /** r || s **/
                             size_t keySize)
{
    mac_state *ms;

    if (NULL == pState || NULL == key)
        return ERR_NULL;

    if (keySize != 32)
        return ERR_KEY_SIZE;
    
    *pState = ms = (mac_state*) calloc(1, sizeof(mac_state));
    if (NULL == ms)
        return ERR_MEMORY;

    poly1305_load_r(ms->r, ms->rr, key);
    memcpy(ms->s, key+16, 16);

    return 0;
}

EXPORT_SYM int poly1305_destroy(mac_state *state)
{
    if (NULL == state)
        return ERR_NULL;
    free(state);
    return 0;
}

EXPORT_SYM int poly1305_update(mac_state *state,
                               const uint8_t *in,
                               size_t len)
{
    if (NULL == state || NULL == in)
        return ERR_NULL;

    while (len>0) {
        unsigned btc;

        btc = (unsigned)MIN(len, 16 - state->buffer_used);
        memcpy(state->buffer + state->buffer_used, in, btc);
        state->buffer_used += btc;
        in += btc;
        len -= btc;

        if (state->buffer_used == 16) {
            poly1305_process(state->h, state->r, state->rr, state->buffer, 16);
            state->buffer_used = 0;
        }
    }

    return 0;
}

EXPORT_SYM int poly1305_digest(const mac_state *state,
                               uint8_t digest[16])
{
    mac_state temp;
    unsigned i;

    if (NULL == state || NULL == digest) {
        return ERR_NULL;
    }

    temp = *state;
    
    if (temp.buffer_used > 0) {
        poly1305_process(temp.h, temp.r, temp.rr, temp.buffer, temp.buffer_used);
    }

    poly1305_finalize(temp.h, temp.s);
    
    for (i=0; i<4; i++) {
        STORE_U32_LITTLE(digest+i*4, temp.h[i]);
    }

    return 0;
}

/*
 * Copyright (C) 2017 Southern Storm Software, Pty Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "skinny64-cipher.h"
#include "skinny-internal.h"

STATIC_INLINE uint32_t skinny64_LFSR2(uint32_t x)
{
    return ((x << 1) & 0xEEEEEEEEU) ^ (((x >> 3) ^ (x >> 2)) & 0x11111111U);
}

STATIC_INLINE uint32_t skinny64_LFSR3(uint32_t x)
{
    return ((x >> 1) & 0x77777777U) ^ ((x ^ (x << 3)) & 0x88888888U);
}

STATIC_INLINE void skinny64_permute_tk(Skinny64Cells_t *tk)
{
    /* PT = [9, 15, 8, 13, 10, 14, 12, 11, 0, 1, 2, 3, 4, 5, 6, 7] */
    uint16_t row2 = tk->row[2];
    uint16_t row3 = tk->row[3];
    tk->row[2] = tk->row[0];
    tk->row[3] = tk->row[1];
    row3 = (row3 << 8) | (row3 >> 8);
    tk->row[0] = ((row2 << 4) & 0x00F0U) |
                 ((row2 << 8) & 0xF000U) |
                  (row3       & 0x0F0FU);
    tk->row[1] = ((row2 >> 8) & 0x00F0U) |
                  (row2       & 0x0F00U) |
                 ((row3 >> 4) & 0x000FU) |
                 ( row3       & 0xF000U);
}

/* Initializes the key schedule with TK1 */
static void skinny64_set_tk1
    (Skinny64Key_t *ks, const void *key, unsigned key_size, int tweaked)
{
    Skinny64Cells_t tk;
    unsigned index;
    uint16_t word;
    uint8_t rc = 0;

    /* Unpack the key and convert from little-endian to host-endian */
    if (key_size >= SKINNY64_BLOCK_SIZE) {
#if SKINNY_64BIT && SKINNY_LITTLE_ENDIAN
        tk.llrow = READ_WORD64(key, 0);
#elif SKINNY_LITTLE_ENDIAN
        tk.lrow[0] = READ_WORD32(key, 0);
        tk.lrow[1] = READ_WORD32(key, 4);
#else
        tk.row[0] = READ_WORD16(key, 0);
        tk.row[1] = READ_WORD16(key, 2);
        tk.row[2] = READ_WORD16(key, 4);
        tk.row[3] = READ_WORD16(key, 6);
#endif
    } else {
        for (index = 0; index < key_size; index += 2) {
            if ((index + 2) <= key_size) {
                word = READ_WORD16(key, index);
            } else {
                word = READ_BYTE(key, index);
            }
            tk.row[index / 2] = word;
        }
    }

    /* Generate the key schedule words for all rounds */
    for (index = 0; index < ks->rounds; ++index) {
        /* Determine the subkey to use at this point in the key schedule */
        ks->schedule[index].lrow = tk.lrow[0];

        /* XOR in the round constants for the first two rows.
           The round constants for the 3rd and 4th rows are
           fixed and will be applied during encrypt/decrypt */
        rc = (rc << 1) ^ ((rc >> 5) & 0x01) ^ ((rc >> 4) & 0x01) ^ 0x01;
        rc &= 0x3F;
        ks->schedule[index].row[0] ^= ((rc & 0x0F) << 4);
        ks->schedule[index].row[1] ^= (rc & 0x30);

        /* If we have a tweak, then we need to XOR a 1 bit into the
           second bit of the top cell of the third column as recommended
           by the SKINNY specification */
        if (tweaked)
            ks->schedule[index].row[0] ^= 0x2000;

        /* Permute TK1 for the next round */
        skinny64_permute_tk(&tk);
    }
}

/* XOR the key schedule with TK1 */
static void skinny64_xor_tk1(Skinny64Key_t *ks, const void *key)
{
    Skinny64Cells_t tk;
    unsigned index;

    /* Unpack the key and convert from little-endian to host-endian */
#if SKINNY_64BIT && SKINNY_LITTLE_ENDIAN
    tk.llrow = READ_WORD64(key, 0);
#elif SKINNY_LITTLE_ENDIAN
    tk.lrow[0] = READ_WORD32(key, 0);
    tk.lrow[1] = READ_WORD32(key, 4);
#else
    tk.row[0] = READ_WORD16(key, 0);
    tk.row[1] = READ_WORD16(key, 2);
    tk.row[2] = READ_WORD16(key, 4);
    tk.row[3] = READ_WORD16(key, 6);
#endif

    /* Generate the key schedule words for all rounds */
    for (index = 0; index < ks->rounds; ++index) {
        /* Determine the subkey to use at this point in the key schedule */
        ks->schedule[index].lrow ^= tk.lrow[0];

        /* Permute TK1 for the next round */
        skinny64_permute_tk(&tk);
    }
}

/* XOR the key schedule with TK2 */
static void skinny64_set_tk2
    (Skinny64Key_t *ks, const void *key, unsigned key_size)
{
    Skinny64Cells_t tk;
    unsigned index;
    uint16_t word;

    /* Unpack the key and convert from little-endian to host-endian */
    if (key_size >= SKINNY64_BLOCK_SIZE) {
#if SKINNY_64BIT && SKINNY_LITTLE_ENDIAN
        tk.llrow = READ_WORD64(key, 0);
#elif SKINNY_LITTLE_ENDIAN
        tk.lrow[0] = READ_WORD32(key, 0);
        tk.lrow[1] = READ_WORD32(key, 4);
#else
        tk.row[0] = READ_WORD16(key, 0);
        tk.row[1] = READ_WORD16(key, 2);
        tk.row[2] = READ_WORD16(key, 4);
        tk.row[3] = READ_WORD16(key, 6);
#endif
    } else {
        for (index = 0; index < key_size; index += 2) {
            if ((index + 2) <= key_size) {
                word = READ_WORD16(key, index);
            } else {
                word = READ_BYTE(key, index);
            }
            tk.row[index / 2] = word;
        }
    }

    /* Generate the key schedule words for all rounds */
    for (index = 0; index < ks->rounds; ++index) {
        /* Determine the subkey to use at this point in the key schedule */
        ks->schedule[index].lrow ^= tk.lrow[0];

        /* Permute TK2 for the next round */
        skinny64_permute_tk(&tk);

        /* Apply LFSR2 to the first two rows of TK2 */
        tk.lrow[0] = skinny64_LFSR2(tk.lrow[0]);
    }
}

/* XOR the key schedule with TK3 */
static void skinny64_set_tk3
    (Skinny64Key_t *ks, const void *key, unsigned key_size)
{
    Skinny64Cells_t tk;
    unsigned index;
    uint16_t word;

    /* Unpack the key and convert from little-endian to host-endian */
    if (key_size >= SKINNY64_BLOCK_SIZE) {
#if SKINNY_64BIT && SKINNY_LITTLE_ENDIAN
        tk.llrow = READ_WORD64(key, 0);
#elif SKINNY_LITTLE_ENDIAN
        tk.lrow[0] = READ_WORD32(key, 0);
        tk.lrow[1] = READ_WORD32(key, 4);
#else
        tk.row[0] = READ_WORD16(key, 0);
        tk.row[1] = READ_WORD16(key, 2);
        tk.row[2] = READ_WORD16(key, 4);
        tk.row[3] = READ_WORD16(key, 6);
#endif
    } else {
        for (index = 0; index < key_size; index += 2) {
            if ((index + 2) <= key_size) {
                word = READ_WORD16(key, index);
            } else {
                word = READ_BYTE(key, index);
            }
            tk.row[index / 2] = word;
        }
    }

    /* Generate the key schedule words for all rounds */
    for (index = 0; index < ks->rounds; ++index) {
        /* Determine the subkey to use at this point in the key schedule */
        ks->schedule[index].lrow ^= tk.lrow[0];

        /* Permute TK3 for the next round */
        skinny64_permute_tk(&tk);

        /* Apply LFSR3 to the first two rows of TK3 */
        tk.lrow[0] = skinny64_LFSR3(tk.lrow[0]);
    }
}

static void skinny64_set_key_inner
    (Skinny64Key_t *ks, const void *key, unsigned key_size, const void *tweak)
{
    if (!tweak) {
        /* Key only, no tweak */
        if (key_size == SKINNY64_BLOCK_SIZE) {
            ks->rounds = 32;
            skinny64_set_tk1(ks, key, key_size, 0);
        } else if (key_size <= (2 * SKINNY64_BLOCK_SIZE)) {
            ks->rounds = 36;
            skinny64_set_tk1(ks, key, SKINNY64_BLOCK_SIZE, 0);
            skinny64_set_tk2(ks, key + SKINNY64_BLOCK_SIZE,
                             key_size - SKINNY64_BLOCK_SIZE);
        } else {
            ks->rounds = 40;
            skinny64_set_tk1(ks, key, SKINNY64_BLOCK_SIZE, 0);
            skinny64_set_tk2(ks, key + SKINNY64_BLOCK_SIZE,
                             SKINNY64_BLOCK_SIZE);
            skinny64_set_tk3(ks, key + SKINNY64_BLOCK_SIZE * 2,
                             key_size - SKINNY64_BLOCK_SIZE * 2);
        }
    } else {
        /* Key and tweak */
        if (key_size == SKINNY64_BLOCK_SIZE) {
            ks->rounds = 36;
            skinny64_set_tk1(ks, tweak, SKINNY64_BLOCK_SIZE, 1);
            skinny64_set_tk2(ks, key, key_size);
        } else {
            ks->rounds = 40;
            skinny64_set_tk1(ks, tweak, SKINNY64_BLOCK_SIZE, 1);
            skinny64_set_tk2(ks, key, SKINNY64_BLOCK_SIZE);
            skinny64_set_tk3(ks, key + SKINNY64_BLOCK_SIZE,
                             key_size - SKINNY64_BLOCK_SIZE);
        }
    }
}

int skinny64_set_key(Skinny64Key_t *ks, const void *key, unsigned size)
{
    /* Validate the parameters */
    if (!ks || !key || size < SKINNY64_BLOCK_SIZE ||
            size > (SKINNY64_BLOCK_SIZE * 3)) {
        return 0;
    }

    /* Set the key directly with no tweak */
    skinny64_set_key_inner(ks, key, size, 0);
    return 1;
}

int skinny64_set_tweaked_key
    (Skinny64TweakedKey_t *ks, const void *key, unsigned key_size)
{
    /* Validate the parameters */
    if (!ks || !key || key_size < SKINNY64_BLOCK_SIZE ||
            key_size > (SKINNY64_BLOCK_SIZE * 2)) {
        return 0;
    }

    /* Set the initial tweak to all-zeroes */
    memset(ks->tweak, 0, sizeof(ks->tweak));

    /* Set the initial key and tweak value */
    skinny64_set_key_inner(&(ks->ks), key, key_size, ks->tweak);
    return 1;
}

int skinny64_set_tweak
    (Skinny64TweakedKey_t *ks, const void *tweak, unsigned tweak_size)
{
    uint8_t tk_prev[SKINNY64_BLOCK_SIZE];

    /* Validate the parameters */
    if (!ks || tweak_size < 1 || tweak_size > SKINNY64_BLOCK_SIZE) {
        return 0;
    }

    /* Read the new tweak value and swap with the original */
    memcpy(tk_prev, ks->tweak, sizeof(tk_prev));
    memcpy(ks->tweak, tweak, tweak_size);
    memset(ks->tweak + tweak_size, 0, sizeof(ks->tweak) - tweak_size);

    /* XOR the original tweak out of the key schedule */
    skinny64_xor_tk1(&(ks->ks), tk_prev);

    /* XOR the new tweak into the key schedule */
    skinny64_xor_tk1(&(ks->ks), ks->tweak);
    return 1;
}

STATIC_INLINE uint16_t skinny64_rotate_right(uint16_t x, unsigned count)
{
    return (x >> count) | (x << (16 - count));
}

#if SKINNY_64BIT

STATIC_INLINE uint64_t skinny64_sbox(uint64_t x)
{
    /* Splitting the bits out individually gives better performance on
       64-bit platforms because we have more spare registers to work with.
       This doesn't work as well on 32-bit platforms or with SIMD because
       register spills start to impact performance.  See below. */
    uint64_t bit0 = x;
    uint64_t bit1 = x >> 1;
    uint64_t bit2 = x >> 2;
    uint64_t bit3 = x >> 3;
    bit0 ^= ~(bit3 | bit2);
    bit3 ^= ~(bit1 | bit2);
    bit2 ^= ~(bit1 | bit0);
    bit1 ^= ~(bit0 | bit3);
    return ((bit0 << 3) & 0x8888888888888888ULL) |
           ( bit1       & 0x1111111111111111ULL) |
           ((bit2 << 1) & 0x2222222222222222ULL) |
           ((bit3 << 2) & 0x4444444444444444ULL);
}

STATIC_INLINE uint64_t skinny64_inv_sbox(uint64_t x)
{
    uint64_t bit0 = x;
    uint64_t bit1 = x >> 1;
    uint64_t bit2 = x >> 2;
    uint64_t bit3 = x >> 3;
    bit0 ^= ~(bit3 | bit2);
    bit1 ^= ~(bit3 | bit0);
    bit2 ^= ~(bit1 | bit0);
    bit3 ^= ~(bit1 | bit2);
    return ((bit0 << 1) & 0x2222222222222222ULL) |
           ((bit1 << 2) & 0x4444444444444444ULL) |
           ((bit2 << 3) & 0x8888888888888888ULL) |
           ( bit3       & 0x1111111111111111ULL);
}

#else

STATIC_INLINE uint32_t skinny64_sbox(uint32_t x)
{
    /* Original version from the specification is equivalent to:
     *
     * #define SBOX_MIX(x)
     *     (((~((((x) >> 1) | (x)) >> 2)) & 0x11111111U) ^ (x))
     * #define SBOX_SHIFT(x)
     *     ((((x) << 1) & 0xEEEEEEEEU) | (((x) >> 3) & 0x11111111U))
     *
     * x = SBOX_MIX(x);
     * x = SBOX_SHIFT(x);
     * x = SBOX_MIX(x);
     * x = SBOX_SHIFT(x);
     * x = SBOX_MIX(x);
     * x = SBOX_SHIFT(x);
     * return SBOX_MIX(x);
     *
     * However, we can mix the bits in their original positions and then
     * delay the SBOX_SHIFT steps to be performed with one final rotation.
     * This reduces the number of required shift operations from 14 to 10.
     *
     * It is possible to reduce the number of shifts and AND's even further
     * as shown in the 64-bit version of skinny64_sbox() above.  However on
     * 32-bit platforms this causes extra register spills which slows down
     * the implementation more than the improvement gained by reducing the
     * number of bit operations.
     */
    x = ((~((x >> 3) | (x >> 2))) & 0x11111111U) ^ x;
    x = ((~((x << 1) | (x << 2))) & 0x88888888U) ^ x;
    x = ((~((x << 1) | (x << 2))) & 0x44444444U) ^ x;
    x = ((~((x >> 2) | (x << 1))) & 0x22222222U) ^ x;
    return ((x >> 1) & 0x77777777U) | ((x << 3) & 0x88888888U);
}

STATIC_INLINE uint32_t skinny64_inv_sbox(uint32_t x)
{
    /* Original version from the specification is equivalent to:
     *
     * #define SBOX_MIX(x)
     *     (((~((((x) >> 1) | (x)) >> 2)) & 0x11111111U) ^ (x))
     * #define SBOX_SHIFT_INV(x)
     *     ((((x) >> 1) & 0x77777777U) | (((x) << 3) & 0x88888888U))
     *
     * x = SBOX_MIX(x);
     * x = SBOX_SHIFT_INV(x);
     * x = SBOX_MIX(x);
     * x = SBOX_SHIFT_INV(x);
     * x = SBOX_MIX(x);
     * x = SBOX_SHIFT_INV(x);
     * return SBOX_MIX(x);
     *
     * However, we can mix the bits in their original positions and then
     * delay the SBOX_SHIFT_INV steps to be performed with one final rotation.
     * This reduces the number of required shift operations from 14 to 10.
     */
    x = ((~((x >> 3) | (x >> 2))) & 0x11111111U) ^ x;
    x = ((~((x << 1) | (x >> 2))) & 0x22222222U) ^ x;
    x = ((~((x << 1) | (x << 2))) & 0x44444444U) ^ x;
    x = ((~((x << 1) | (x << 2))) & 0x88888888U) ^ x;
    return ((x << 1) & 0xEEEEEEEEU) | ((x >> 3) & 0x11111111U);
}

#endif

void skinny64_ecb_encrypt
    (void *output, const void *input, const Skinny64Key_t *ks)
{
    Skinny64Cells_t state;
    const Skinny64HalfCells_t *schedule;
    unsigned index;
    uint32_t temp;

    /* Read the input buffer and convert little-endian to host-endian */
#if SKINNY_64BIT && SKINNY_LITTLE_ENDIAN
    state.llrow = READ_WORD64(input, 0);
#elif SKINNY_LITTLE_ENDIAN
    state.lrow[0] = READ_WORD32(input, 0);
    state.lrow[1] = READ_WORD32(input, 4);
#else
    state.row[0] = READ_WORD16(input, 0);
    state.row[1] = READ_WORD16(input, 2);
    state.row[2] = READ_WORD16(input, 4);
    state.row[3] = READ_WORD16(input, 6);
#endif

    /* Perform all encryption rounds */
    schedule = ks->schedule;
    for (index = ks->rounds; index > 0; --index, ++schedule) {
        /* Apply the S-box to all bytes in the state */
#if SKINNY_64BIT
        state.llrow = skinny64_sbox(state.llrow);
#else
        state.lrow[0] = skinny64_sbox(state.lrow[0]);
        state.lrow[1] = skinny64_sbox(state.lrow[1]);
#endif

        /* Apply the subkey for this round */
#if SKINNY_64BIT && SKINNY_LITTLE_ENDIAN
        state.llrow ^= schedule->lrow | 0x2000000000ULL;
#else
        state.lrow[0] ^= schedule->lrow;
        state.row[2] ^= 0x20;
#endif

        /* Shift the rows */
        state.row[1] = skinny64_rotate_right(state.row[1], 4);
        state.row[2] = skinny64_rotate_right(state.row[2], 8);
        state.row[3] = skinny64_rotate_right(state.row[3], 12);

        /* Mix the columns */
        state.row[1] ^= state.row[2];
        state.row[2] ^= state.row[0];
        temp = state.row[3] ^ state.row[2];
        state.row[3] = state.row[2];
        state.row[2] = state.row[1];
        state.row[1] = state.row[0];
        state.row[0] = temp;
    }

    /* Convert host-endian back into little-endian in the output buffer */
#if SKINNY_64BIT && SKINNY_LITTLE_ENDIAN
    WRITE_WORD64(output, 0, state.llrow);
#elif SKINNY_LITTLE_ENDIAN
    WRITE_WORD32(output, 0, state.lrow[0]);
    WRITE_WORD32(output, 4, state.lrow[1]);
#else
    WRITE_WORD16(output, 0, state.row[0]);
    WRITE_WORD16(output, 2, state.row[1]);
    WRITE_WORD16(output, 4, state.row[2]);
    WRITE_WORD16(output, 6, state.row[3]);
#endif
}

void skinny64_ecb_decrypt
    (void *output, const void *input, const Skinny64Key_t *ks)
{
    Skinny64Cells_t state;
    const Skinny64HalfCells_t *schedule;
    unsigned index;
    uint32_t temp;

    /* Read the input buffer and convert little-endian to host-endian */
#if SKINNY_64BIT && SKINNY_LITTLE_ENDIAN
    state.llrow = READ_WORD64(input, 0);
#elif SKINNY_LITTLE_ENDIAN
    state.lrow[0] = READ_WORD32(input, 0);
    state.lrow[1] = READ_WORD32(input, 4);
#else
    state.row[0] = READ_WORD16(input, 0);
    state.row[1] = READ_WORD16(input, 2);
    state.row[2] = READ_WORD16(input, 4);
    state.row[3] = READ_WORD16(input, 6);
#endif

    /* Perform all decryption rounds */
    schedule = &(ks->schedule[ks->rounds - 1]);
    for (index = ks->rounds; index > 0; --index, --schedule) {
        /* Inverse mix of the columns */
        temp = state.row[3];
        state.row[3] = state.row[0];
        state.row[0] = state.row[1];
        state.row[1] = state.row[2];
        state.row[3] ^= temp;
        state.row[2] = temp ^ state.row[0];
        state.row[1] ^= state.row[2];

        /* Inverse shift of the rows */
        state.row[1] = skinny64_rotate_right(state.row[1], 12);
        state.row[2] = skinny64_rotate_right(state.row[2], 8);
        state.row[3] = skinny64_rotate_right(state.row[3], 4);

        /* Apply the subkey for this round */
#if SKINNY_64BIT && SKINNY_LITTLE_ENDIAN
        state.llrow ^= schedule->lrow | 0x2000000000ULL;
#else
        state.lrow[0] ^= schedule->lrow;
        state.row[2] ^= 0x20;
#endif

        /* Apply the inverse of the S-box to all bytes in the state */
#if SKINNY_64BIT
        state.llrow = skinny64_inv_sbox(state.llrow);
#else
        state.lrow[0] = skinny64_inv_sbox(state.lrow[0]);
        state.lrow[1] = skinny64_inv_sbox(state.lrow[1]);
#endif
    }

    /* Convert host-endian back into little-endian in the output buffer */
#if SKINNY_64BIT && SKINNY_LITTLE_ENDIAN
    WRITE_WORD64(output, 0, state.llrow);
#elif SKINNY_LITTLE_ENDIAN
    WRITE_WORD32(output, 0, state.lrow[0]);
    WRITE_WORD32(output, 4, state.lrow[1]);
#else
    WRITE_WORD16(output, 0, state.row[0]);
    WRITE_WORD16(output, 2, state.row[1]);
    WRITE_WORD16(output, 4, state.row[2]);
    WRITE_WORD16(output, 6, state.row[3]);
#endif
}

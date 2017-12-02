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

#include "skinny128-cipher.h"
#include "skinny-internal.h"

#if SKINNY_64BIT

STATIC_INLINE uint64_t skinny128_LFSR2(uint64_t x)
{
    return ((x << 1) & 0xFEFEFEFEFEFEFEFEULL) ^
           (((x >> 7) ^ (x >> 5)) & 0x0101010101010101ULL);
}

STATIC_INLINE uint64_t skinny128_LFSR3(uint64_t x)
{
    return ((x >> 1) & 0x7F7F7F7F7F7F7F7FULL) ^
           (((x << 7) ^ (x << 1)) & 0x8080808080808080ULL);
}

#else

STATIC_INLINE uint32_t skinny128_LFSR2(uint32_t x)
{
    return ((x << 1) & 0xFEFEFEFEU) ^ (((x >> 7) ^ (x >> 5)) & 0x01010101U);
}

STATIC_INLINE uint32_t skinny128_LFSR3(uint32_t x)
{
    return ((x >> 1) & 0x7F7F7F7FU) ^ (((x << 7) ^ (x << 1)) & 0x80808080U);
}

#endif

STATIC_INLINE void skinny128_permute_tk(Skinny128Cells_t *tk)
{
    /* PT = [9, 15, 8, 13, 10, 14, 12, 11, 0, 1, 2, 3, 4, 5, 6, 7] */
    uint32_t row2 = tk->row[2];
    uint32_t row3 = tk->row[3];
    tk->row[2] = tk->row[0];
    tk->row[3] = tk->row[1];
    row3 = (row3 << 16) | (row3 >> 16);
    tk->row[0] = ((row2 >>  8) & 0x000000FFU) |
                 ((row2 << 16) & 0x00FF0000U) |
                 ( row3        & 0xFF00FF00U);
    tk->row[1] = ((row2 >> 16) & 0x000000FFU) |
                  (row2        & 0xFF000000U) |
                 ((row3 <<  8) & 0x0000FF00U) |
                 ( row3        & 0x00FF0000U);
}

/* Initializes the key schedule with TK1 */
static void skinny128_set_tk1
    (Skinny128Key_t *ks, const void *key, unsigned key_size, int tweaked)
{
    Skinny128Cells_t tk;
    unsigned index;
    uint16_t word;
    uint8_t rc = 0;

    /* Unpack the key and convert from little-endian to host-endian */
    if (key_size >= SKINNY128_BLOCK_SIZE) {
        tk.row[0] = READ_WORD32(key, 0);
        tk.row[1] = READ_WORD32(key, 4);
        tk.row[2] = READ_WORD32(key, 8);
        tk.row[3] = READ_WORD32(key, 12);
    } else {
        for (index = 0; index < key_size; index += 4) {
            if ((index + 4) <= key_size) {
                word = READ_WORD32(key, index);
            } else {
                word = READ_BYTE(key, index);
                if ((index + 1) < key_size)
                    word |= (READ_BYTE(key, index + 1) << 8);
                if ((index + 2) < key_size)
                    word |= (READ_BYTE(key, index + 2) << 16);
            }
            tk.row[index / 4] = word;
        }
    }

    /* Generate the key schedule words for all rounds */
    for (index = 0; index < ks->rounds; ++index) {
        /* Determine the subkey to use at this point in the key schedule */
#if SKINNY_64BIT
        ks->schedule[index].lrow = tk.lrow[0];
#else
        ks->schedule[index].row[0] = tk.row[0];
        ks->schedule[index].row[1] = tk.row[1];
#endif

        /* XOR in the round constants for the first two rows.
           The round constants for the 3rd and 4th rows are
           fixed and will be applied during encrypt/decrypt */
        rc = (rc << 1) ^ ((rc >> 5) & 0x01) ^ ((rc >> 4) & 0x01) ^ 0x01;
        rc &= 0x3F;
        ks->schedule[index].row[0] ^= (rc & 0x0F);
        ks->schedule[index].row[1] ^= (rc >> 4);

        /* If we have a tweak, then we need to XOR a 1 bit into the
           second bit of the top cell of the third column as recommended
           by the SKINNY specification */
        if (tweaked)
            ks->schedule[index].row[0] ^= 0x00020000;

        /* Permute TK1 for the next round */
        skinny128_permute_tk(&tk);
    }
}

/* XOR the key schedule with TK1 */
static void skinny128_xor_tk1(Skinny128Key_t *ks, const void *key)
{
    Skinny128Cells_t tk;
    unsigned index;

    /* Unpack the key and convert from little-endian to host-endian */
    tk.row[0] = READ_WORD32(key, 0);
    tk.row[1] = READ_WORD32(key, 4);
    tk.row[2] = READ_WORD32(key, 8);
    tk.row[3] = READ_WORD32(key, 12);

    /* Generate the key schedule words for all rounds */
    for (index = 0; index < ks->rounds; ++index) {
        /* Determine the subkey to use at this point in the key schedule */
#if SKINNY_64BIT
        ks->schedule[index].lrow ^= tk.lrow[0];
#else
        ks->schedule[index].row[0] ^= tk.row[0];
        ks->schedule[index].row[1] ^= tk.row[1];
#endif

        /* Permute TK1 for the next round */
        skinny128_permute_tk(&tk);
    }
}

/* XOR the key schedule with TK2 */
static void skinny128_set_tk2
    (Skinny128Key_t *ks, const void *key, unsigned key_size)
{
    Skinny128Cells_t tk;
    unsigned index;
    uint16_t word;

    /* Unpack the key and convert from little-endian to host-endian */
    if (key_size >= SKINNY128_BLOCK_SIZE) {
        tk.row[0] = READ_WORD32(key, 0);
        tk.row[1] = READ_WORD32(key, 4);
        tk.row[2] = READ_WORD32(key, 8);
        tk.row[3] = READ_WORD32(key, 12);
    } else {
        for (index = 0; index < key_size; index += 4) {
            if ((index + 4) <= key_size) {
                word = READ_WORD32(key, index);
            } else {
                word = READ_BYTE(key, index);
                if ((index + 1) < key_size)
                    word |= (READ_BYTE(key, index + 1) << 8);
                if ((index + 2) < key_size)
                    word |= (READ_BYTE(key, index + 2) << 16);
            }
            tk.row[index / 4] = word;
        }
    }

    /* Generate the key schedule words for all rounds */
    for (index = 0; index < ks->rounds; ++index) {
        /* Determine the subkey to use at this point in the key schedule */
#if SKINNY_64BIT
        ks->schedule[index].lrow ^= tk.lrow[0];
#else
        ks->schedule[index].row[0] ^= tk.row[0];
        ks->schedule[index].row[1] ^= tk.row[1];
#endif

        /* Permute TK2 for the next round */
        skinny128_permute_tk(&tk);

        /* Apply LFSR2 to the first two rows of TK2 */
#if SKINNY_64BIT
        tk.lrow[0] = skinny128_LFSR2(tk.lrow[0]);
#else
        tk.row[0] = skinny128_LFSR2(tk.row[0]);
        tk.row[1] = skinny128_LFSR2(tk.row[1]);
#endif
    }
}

/* XOR the key schedule with TK3 */
static void skinny128_set_tk3
    (Skinny128Key_t *ks, const void *key, unsigned key_size)
{
    Skinny128Cells_t tk;
    unsigned index;
    uint16_t word;

    /* Unpack the key and convert from little-endian to host-endian */
    if (key_size >= SKINNY128_BLOCK_SIZE) {
        tk.row[0] = READ_WORD32(key, 0);
        tk.row[1] = READ_WORD32(key, 4);
        tk.row[2] = READ_WORD32(key, 8);
        tk.row[3] = READ_WORD32(key, 12);
    } else {
        for (index = 0; index < key_size; index += 4) {
            if ((index + 4) <= key_size) {
                word = READ_WORD32(key, index);
            } else {
                word = READ_BYTE(key, index);
                if ((index + 1) < key_size)
                    word |= (READ_BYTE(key, index + 1) << 8);
                if ((index + 2) < key_size)
                    word |= (READ_BYTE(key, index + 2) << 16);
            }
            tk.row[index / 4] = word;
        }
    }

    /* Generate the key schedule words for all rounds */
    for (index = 0; index < ks->rounds; ++index) {
        /* Determine the subkey to use at this point in the key schedule */
#if SKINNY_64BIT
        ks->schedule[index].lrow ^= tk.lrow[0];
#else
        ks->schedule[index].row[0] ^= tk.row[0];
        ks->schedule[index].row[1] ^= tk.row[1];
#endif

        /* Permute TK3 for the next round */
        skinny128_permute_tk(&tk);

        /* Apply LFSR3 to the first two rows of TK2 */
#if SKINNY_64BIT
        tk.lrow[0] = skinny128_LFSR3(tk.lrow[0]);
#else
        tk.row[0] = skinny128_LFSR3(tk.row[0]);
        tk.row[1] = skinny128_LFSR3(tk.row[1]);
#endif
    }
}

static void skinny128_set_key_inner
    (Skinny128Key_t *ks, const void *key, unsigned key_size, const void *tweak)
{
    if (!tweak) {
        /* Key only, no tweak */
        if (key_size == SKINNY128_BLOCK_SIZE) {
            ks->rounds = 40;
            skinny128_set_tk1(ks, key, key_size, 0);
        } else if (key_size <= (2 * SKINNY128_BLOCK_SIZE)) {
            ks->rounds = 48;
            skinny128_set_tk1(ks, key, SKINNY128_BLOCK_SIZE, 0);
            skinny128_set_tk2(ks, key + SKINNY128_BLOCK_SIZE,
                              key_size - SKINNY128_BLOCK_SIZE);
        } else {
            ks->rounds = 56;
            skinny128_set_tk1(ks, key, SKINNY128_BLOCK_SIZE, 0);
            skinny128_set_tk2(ks, key + SKINNY128_BLOCK_SIZE,
                              SKINNY128_BLOCK_SIZE);
            skinny128_set_tk3(ks, key + SKINNY128_BLOCK_SIZE * 2,
                              key_size - SKINNY128_BLOCK_SIZE * 2);
        }
    } else {
        /* Key and tweak */
        if (key_size == SKINNY128_BLOCK_SIZE) {
            ks->rounds = 48;
            skinny128_set_tk1(ks, tweak, SKINNY128_BLOCK_SIZE, 1);
            skinny128_set_tk2(ks, key, key_size);
        } else {
            ks->rounds = 56;
            skinny128_set_tk1(ks, tweak, SKINNY128_BLOCK_SIZE, 1);
            skinny128_set_tk2(ks, key, SKINNY128_BLOCK_SIZE);
            skinny128_set_tk3(ks, key + SKINNY128_BLOCK_SIZE,
                              key_size - SKINNY128_BLOCK_SIZE);
        }
    }
}

int skinny128_set_key(Skinny128Key_t *ks, const void *key, unsigned size)
{
    /* Validate the parameters */
    if (!ks || !key || size < SKINNY128_BLOCK_SIZE ||
            size > (SKINNY128_BLOCK_SIZE * 3)) {
        return 0;
    }

    /* Set the key directly with no tweak */
    skinny128_set_key_inner(ks, key, size, 0);
    return 1;
}

int skinny128_set_tweaked_key
    (Skinny128TweakedKey_t *ks, const void *key, unsigned key_size)
{
    /* Validate the parameters */
    if (!ks || !key || key_size < SKINNY128_BLOCK_SIZE ||
            key_size > (SKINNY128_BLOCK_SIZE * 2)) {
        return 0;
    }

    /* Set the initial tweak to all-zeroes */
    memset(ks->tweak, 0, sizeof(ks->tweak));

    /* Set the initial key and tweak value */
    skinny128_set_key_inner(&(ks->ks), key, key_size, ks->tweak);
    return 1;
}

int skinny128_set_tweak
    (Skinny128TweakedKey_t *ks, const void *tweak, unsigned tweak_size)
{
    uint8_t tk_prev[SKINNY128_BLOCK_SIZE];

    /* Validate the parameters */
    if (!ks || tweak_size < 1 || tweak_size > SKINNY128_BLOCK_SIZE) {
        return 0;
    }

    /* Read the new tweak value and swap with the original */
    memcpy(tk_prev, ks->tweak, sizeof(tk_prev));
    memcpy(ks->tweak, tweak, tweak_size);
    memset(ks->tweak + tweak_size, 0, sizeof(ks->tweak) - tweak_size);

    /* XOR the original tweak out of the key schedule */
    skinny128_xor_tk1(&(ks->ks), tk_prev);

    /* XOR the new tweak into the key schedule */
    skinny128_xor_tk1(&(ks->ks), ks->tweak);
    return 1;
}

STATIC_INLINE uint32_t skinny128_rotate_right(uint32_t x, unsigned count)
{
    /* Note: we are rotating the cells right, which actually moves
       the values up closer to the MSB.  That is, we do a left shift
       on the word to rotate the cells in the word right */
    return (x << count) | (x >> (32 - count));
}

#if SKINNY_64BIT

STATIC_INLINE uint64_t skinny128_sbox(uint64_t x)
{
    /* See 32-bit version below for a description of what is happening here */
    uint64_t y;
    x ^= ((~((x >> 2) | (x >> 3))) & 0x1111111111111111ULL);
    y  = ((~((x << 5) | (x << 1))) & 0x2020202020202020ULL);
    x ^= ((~((x << 5) | (x << 4))) & 0x4040404040404040ULL) ^ y;
    y  = ((~((x << 2) | (x << 1))) & 0x8080808080808080ULL);
    x ^= ((~((x >> 2) | (x << 1))) & 0x0202020202020202ULL) ^ y;
    y  = ((~((x >> 5) | (x << 1))) & 0x0404040404040404ULL);
    x ^= ((~((x >> 1) | (x >> 2))) & 0x0808080808080808ULL) ^ y;
    return ((x & 0x0808080808080808ULL) << 1) |
           ((x & 0x3232323232323232ULL) << 2) |
           ((x & 0x0101010101010101ULL) << 5) |
           ((x & 0x8080808080808080ULL) >> 6) |
           ((x & 0x4040404040404040ULL) >> 4) |
           ((x & 0x0404040404040404ULL) >> 2);
}

STATIC_INLINE uint64_t skinny128_inv_sbox(uint64_t x)
{
    /* See 32-bit version below for a description of what is happening here */
    uint64_t y;
    y  = ((~((x >> 1) | (x >> 3))) & 0x0101010101010101ULL);
    x ^= ((~((x >> 2) | (x >> 3))) & 0x1010101010101010ULL) ^ y;
    y  = ((~((x >> 6) | (x >> 1))) & 0x0202020202020202ULL);
    x ^= ((~((x >> 1) | (x >> 2))) & 0x0808080808080808ULL) ^ y;
    y  = ((~((x << 2) | (x << 1))) & 0x8080808080808080ULL);
    x ^= ((~((x >> 1) | (x << 2))) & 0x0404040404040404ULL) ^ y;
    y  = ((~((x << 5) | (x << 1))) & 0x2020202020202020ULL);
    x ^= ((~((x << 4) | (x << 5))) & 0x4040404040404040ULL) ^ y;
    return ((x & 0x0101010101010101ULL) << 2) |
           ((x & 0x0404040404040404ULL) << 4) |
           ((x & 0x0202020202020202ULL) << 6) |
           ((x & 0x2020202020202020ULL) >> 5) |
           ((x & 0xC8C8C8C8C8C8C8C8ULL) >> 2) |
           ((x & 0x1010101010101010ULL) >> 1);
}

#else

STATIC_INLINE uint32_t skinny128_sbox(uint32_t x)
{
    /* Original version from the specification is equivalent to:
     *
     * #define SBOX_MIX(x)
     *     (((~((((x) >> 1) | (x)) >> 2)) & 0x11111111U) ^ (x))
     * #define SBOX_SWAP(x)
     *     (((x) & 0xF9F9F9F9U) |
     *     (((x) >> 1) & 0x02020202U) |
     *     (((x) << 1) & 0x04040404U))
     * #define SBOX_PERMUTE(x)
     *     ((((x) & 0x01010101U) << 2) |
     *      (((x) & 0x06060606U) << 5) |
     *      (((x) & 0x20202020U) >> 5) |
     *      (((x) & 0xC8C8C8C8U) >> 2) |
     *      (((x) & 0x10101010U) >> 1))
     *
     * x = SBOX_MIX(x);
     * x = SBOX_PERMUTE(x);
     * x = SBOX_MIX(x);
     * x = SBOX_PERMUTE(x);
     * x = SBOX_MIX(x);
     * x = SBOX_PERMUTE(x);
     * x = SBOX_MIX(x);
     * return SBOX_SWAP(x);
     *
     * However, we can mix the bits in their original positions and then
     * delay the SBOX_PERMUTE and SBOX_SWAP steps to be performed with one
     * final permuatation.  This reduces the number of shift operations.
     */
    uint32_t y;

    /* Mix the bits */
    x ^= ((~((x >> 2) | (x >> 3))) & 0x11111111U);
    y  = ((~((x << 5) | (x << 1))) & 0x20202020U);
    x ^= ((~((x << 5) | (x << 4))) & 0x40404040U) ^ y;
    y  = ((~((x << 2) | (x << 1))) & 0x80808080U);
    x ^= ((~((x >> 2) | (x << 1))) & 0x02020202U) ^ y;
    y  = ((~((x >> 5) | (x << 1))) & 0x04040404U);
    x ^= ((~((x >> 1) | (x >> 2))) & 0x08080808U) ^ y;

    /* Permutation generated by http://programming.sirrida.de/calcperm.php
       The final permutation for each byte is [2 7 6 1 3 0 4 5] */
    return ((x & 0x08080808U) << 1) |
           ((x & 0x32323232U) << 2) |
           ((x & 0x01010101U) << 5) |
           ((x & 0x80808080U) >> 6) |
           ((x & 0x40404040U) >> 4) |
           ((x & 0x04040404U) >> 2);
}

STATIC_INLINE uint32_t skinny128_inv_sbox(uint32_t x)
{
    /* Original version from the specification is equivalent to:
     *
     * #define SBOX_MIX(x)
     *     (((~((((x) >> 1) | (x)) >> 2)) & 0x11111111U) ^ (x))
     * #define SBOX_SWAP(x)
     *     (((x) & 0xF9F9F9F9U) |
     *     (((x) >> 1) & 0x02020202U) |
     *     (((x) << 1) & 0x04040404U))
     * #define SBOX_PERMUTE_INV(x)
     *     ((((x) & 0x08080808U) << 1) |
     *      (((x) & 0x32323232U) << 2) |
     *      (((x) & 0x01010101U) << 5) |
     *      (((x) & 0xC0C0C0C0U) >> 5) |
     *      (((x) & 0x04040404U) >> 2))
     *
     * x = SBOX_SWAP(x);
     * x = SBOX_MIX(x);
     * x = SBOX_PERMUTE_INV(x);
     * x = SBOX_MIX(x);
     * x = SBOX_PERMUTE_INV(x);
     * x = SBOX_MIX(x);
     * x = SBOX_PERMUTE_INV(x);
     * return SBOX_MIX(x);
     *
     * However, we can mix the bits in their original positions and then
     * delay the SBOX_PERMUTE_INV and SBOX_SWAP steps to be performed with one
     * final permuatation.  This reduces the number of shift operations.
     */
    uint32_t y;

    /* Mix the bits */
    y  = ((~((x >> 1) | (x >> 3))) & 0x01010101U);
    x ^= ((~((x >> 2) | (x >> 3))) & 0x10101010U) ^ y;
    y  = ((~((x >> 6) | (x >> 1))) & 0x02020202U);
    x ^= ((~((x >> 1) | (x >> 2))) & 0x08080808U) ^ y;
    y  = ((~((x << 2) | (x << 1))) & 0x80808080U);
    x ^= ((~((x >> 1) | (x << 2))) & 0x04040404U) ^ y;
    y  = ((~((x << 5) | (x << 1))) & 0x20202020U);
    x ^= ((~((x << 4) | (x << 5))) & 0x40404040U) ^ y;

    /* Permutation generated by http://programming.sirrida.de/calcperm.php
       The final permutation for each byte is [5 3 0 4 6 7 2 1] */
    return ((x & 0x01010101U) << 2) |
           ((x & 0x04040404U) << 4) |
           ((x & 0x02020202U) << 6) |
           ((x & 0x20202020U) >> 5) |
           ((x & 0xC8C8C8C8U) >> 2) |
           ((x & 0x10101010U) >> 1);
}

#endif

void skinny128_ecb_encrypt
    (void *output, const void *input, const Skinny128Key_t *ks)
{
    Skinny128Cells_t state;
    const Skinny128HalfCells_t *schedule;
    unsigned index;
    uint32_t temp;

    /* Read the input buffer and convert little-endian to host-endian */
    state.row[0] = READ_WORD32(input, 0);
    state.row[1] = READ_WORD32(input, 4);
    state.row[2] = READ_WORD32(input, 8);
    state.row[3] = READ_WORD32(input, 12);

    /* Perform all encryption rounds */
    schedule = ks->schedule;
    for (index = ks->rounds; index > 0; --index, ++schedule) {
        /* Apply the S-box to all bytes in the state */
#if SKINNY_64BIT
        state.lrow[0] = skinny128_sbox(state.lrow[0]);
        state.lrow[1] = skinny128_sbox(state.lrow[1]);
#else
        state.row[0] = skinny128_sbox(state.row[0]);
        state.row[1] = skinny128_sbox(state.row[1]);
        state.row[2] = skinny128_sbox(state.row[2]);
        state.row[3] = skinny128_sbox(state.row[3]);
#endif

        /* Apply the subkey for this round */
#if SKINNY_64BIT
        state.lrow[0] ^= schedule->lrow;
        state.lrow[1] ^= 0x02;
#else
        state.row[0] ^= schedule->row[0];
        state.row[1] ^= schedule->row[1];
        state.row[2] ^= 0x02;
#endif

        /* Shift the rows */
        state.row[1] = skinny128_rotate_right(state.row[1], 8);
        state.row[2] = skinny128_rotate_right(state.row[2], 16);
        state.row[3] = skinny128_rotate_right(state.row[3], 24);

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
    WRITE_WORD32(output, 0, state.row[0]);
    WRITE_WORD32(output, 4, state.row[1]);
    WRITE_WORD32(output, 8, state.row[2]);
    WRITE_WORD32(output, 12, state.row[3]);
}

void skinny128_ecb_decrypt
    (void *output, const void *input, const Skinny128Key_t *ks)
{
    Skinny128Cells_t state;
    const Skinny128HalfCells_t *schedule;
    unsigned index;
    uint32_t temp;

    /* Read the input buffer and convert little-endian to host-endian */
    state.row[0] = READ_WORD32(input, 0);
    state.row[1] = READ_WORD32(input, 4);
    state.row[2] = READ_WORD32(input, 8);
    state.row[3] = READ_WORD32(input, 12);

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
        state.row[1] = skinny128_rotate_right(state.row[1], 24);
        state.row[2] = skinny128_rotate_right(state.row[2], 16);
        state.row[3] = skinny128_rotate_right(state.row[3], 8);

        /* Apply the subkey for this round */
#if SKINNY_64BIT
        state.lrow[0] ^= schedule->lrow;
#else
        state.row[0] ^= schedule->row[0];
        state.row[1] ^= schedule->row[1];
#endif
        state.row[2] ^= 0x02;

        /* Apply the inverse of the S-box to all bytes in the state */
#if SKINNY_64BIT
        state.lrow[0] = skinny128_inv_sbox(state.lrow[0]);
        state.lrow[1] = skinny128_inv_sbox(state.lrow[1]);
#else
        state.row[0] = skinny128_inv_sbox(state.row[0]);
        state.row[1] = skinny128_inv_sbox(state.row[1]);
        state.row[2] = skinny128_inv_sbox(state.row[2]);
        state.row[3] = skinny128_inv_sbox(state.row[3]);
#endif
    }

    /* Convert host-endian back into little-endian in the output buffer */
    WRITE_WORD32(output, 0, state.row[0]);
    WRITE_WORD32(output, 4, state.row[1]);
    WRITE_WORD32(output, 8, state.row[2]);
    WRITE_WORD32(output, 12, state.row[3]);
}

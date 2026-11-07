/*
 * nr_softfloat.c - build IEEE-754 binary32 values with integer arithmetic.
 *
 * Context: any.  Pure computation, no locks, no memory access.
 */
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/math.h>
#include <linux/printk.h>

#include "nr_internal.h"

/* Layout of IEEE-754 binary32: [sign:1][biased exponent:8][mantissa:23]. */
#define FLOAT32_MANTISSA_BITS 23
#define FLOAT32_EXPONENT_BIAS 127
#define FLOAT32_MANTISSA_MASK ((u32)(1u << FLOAT32_MANTISSA_BITS) - 1u)

/* A normalised significand occupies [2^23, 2^24): the leading 1 is implicit
 * and only the low 23 bits are stored.  Reaching 2^24 means rounding carried
 * out of the significand and the exponent must absorb it. */
#define FLOAT32_SIGNIFICAND_MIN (1u << FLOAT32_MANTISSA_BITS)
#define FLOAT32_SIGNIFICAND_MAX (1u << (FLOAT32_MANTISSA_BITS + 1))

s32 nr_clamp_fps(s32 requested_fps)
{
    if (requested_fps < NR_FPS_MIN)
    {
        return NR_FPS_MIN;
    }
    if (requested_fps > NR_FPS_MAX)
    {
        return NR_FPS_MAX;
    }
    return requested_fps;
}

/*
 * 1/divisor as binary32, without touching the FPU.
 *
 * With 2^h <= divisor < 2^(h+1) the reciprocal lies in (2^-(h+1), 2^-h], so
 * the normalised exponent is -h for an exact power of two and -(h+1)
 * otherwise.  Scaling the numerator by 2^(23 - exponent) then makes the
 * quotient land directly on the 24-bit significand.
 *
 * Rounding: DIV_ROUND_CLOSEST_ULL rounds halves away from zero, which would
 * differ from IEEE round-half-to-even only on an exact tie.  A tie needs the
 * remainder to be exactly half the divisor, which for a power-of-two-scaled
 * numerator can only happen when divisor is itself a power of two - and then
 * the division is exact and there is no tie at all.  So the two rounding
 * modes agree for every divisor this function accepts.
 */
u32 nr_float32_reciprocal(u32 divisor)
{
    int highest_bit;
    int exponent;
    u64 scaled_numerator;
    u32 significand;

    if (divisor == 0)
    {
        return 0;
    }

    highest_bit = fls(divisor) - 1;
    exponent = is_power_of_2(divisor) ? -highest_bit : -(highest_bit + 1);

    /* exponent is <= 0, so the shift is 23..23+31 and stays inside a u64. */
    scaled_numerator = 1ULL << (FLOAT32_MANTISSA_BITS - exponent);
    significand = (u32)DIV_ROUND_CLOSEST_ULL(scaled_numerator, divisor);

    if (significand >= FLOAT32_SIGNIFICAND_MAX)
    {
        /* Rounding carried past the implicit one, e.g. 0.111...1 -> 1.0. */
        significand >>= 1;
        exponent++;
    }

    return ((u32)(exponent + FLOAT32_EXPONENT_BIAS) << FLOAT32_MANTISSA_BITS) |
           (significand & FLOAT32_MANTISSA_MASK);
}

int nr_softfloat_selftest(void)
{
    /* Reference bit patterns for 1.0f/n, cross-checked against a userspace
     * float build.  60 -> 0x3C888889 is the constant the game itself stores. */
    static const struct
    {
        u32 divisor;
        u32 expected_bits;
    } cases[] = {
        {1, 0x3F800000},   {2, 0x3F000000},   {3, 0x3EAAAAAB},   {30, 0x3D088889},
        {60, 0x3C888889},  {75, 0x3C5A740E},  {90, 0x3C360B61},  {120, 0x3C088889},
        {144, 0x3BE38E39}, {165, 0x3BC6980C}, {240, 0x3B888889}, {360, 0x3B360B61},
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(cases); i++)
    {
        u32 got = nr_float32_reciprocal(cases[i].divisor);

        if (got != cases[i].expected_bits)
        {
            pr_err("nightpatch: softfloat selftest failed: 1/%u = %#010x, expected %#010x\n", cases[i].divisor, got,
                   cases[i].expected_bits);
            return -EINVAL;
        }
    }

    pr_info("nightpatch: softfloat selftest passed (%zu cases)\n", ARRAY_SIZE(cases));
    return 0;
}

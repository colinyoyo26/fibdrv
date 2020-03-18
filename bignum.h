#include <linux/slab.h>

typedef struct bignum {
    unsigned long long *ptr;
    unsigned int size;  //#sizeof(unsigned long long) bytes are used
    unsigned int capacity;
} bn;

/* number of bits of unsigned long long type */
#define N_BITS sizeof(unsigned long long) * 8
/* log_2(N_BITS) */
#define N_BITS_TZ __builtin_ctz(N_BITS)

/* If p is NULL, krealloc behaves exactly like kmalloc */
static inline void bn_resize(bn *a, unsigned long long size)
{
    a->ptr = (unsigned long long *) krealloc(
        a->ptr, size * sizeof(unsigned long long), GFP_KERNEL);
    a->capacity = size;
}

static inline void bn_init(bn *a)
{
    a->size = 0;
    a->capacity = 0;
    a->ptr = NULL;
}

static inline void bn_assign(bn *a, unsigned long long val)
{
    if (a->ptr == NULL)
        bn_resize(a, 1);
    a->ptr[0] = val;
    a->size = 1;
}

static inline unsigned int bn_size(bn *a)
{
    return a->size;
}

static inline unsigned long long bn_capacity(bn *a)
{
    /* equal to ksize(a->ptr) / sizeof(unsigned long long) */
    return a->capacity;
}
static inline void bn_swap(bn **a, bn **b)
{
    bn *tem = *a;
    *a = *b;
    *b = tem;
}

/* retrun non-zero if a is greater than b */
static inline int bn_greater(bn *a, bn *b)
{
#define SIGN_BIT 1ull << (N_BITS - 1)
    if ((bn_size(a) > bn_size(b)))
        return 1;
    for (int i = bn_size(a) - 1; i > -1; i--) {
        if (a->ptr[i] == b->ptr[i])
            continue;
        return (b->ptr[i] - a->ptr[i]) & SIGN_BIT;
    }
    return 0;
#undef SIGN_BIT
}

#define MAX(a, b)         \
    ({                    \
        typeof(a) _a = a; \
        typeof(b) _b = b; \
        _a > _b ? a : b;  \
    })

static inline void bn_add(bn *result, bn *a, bn *b)
{
    unsigned int size = MAX(bn_size(a), bn_size(b));

    /* prevent potential overflow */
    if (bn_capacity(result) < size + 1)
        bn_resize(result, bn_size(a) + 1);

    int i, carry = 0;
    for (i = 0; i < bn_size(b); i++) {
        /* it's fine to overflow */
        result->ptr[i] = a->ptr[i] + b->ptr[i] + carry;
        /* thinking about strength reduction */
        carry = ((a->ptr[i] + carry > ~b->ptr[i]) || (a->ptr[i] > ~carry));
    }
    for (; i < size; i++) {
        result->ptr[i] = a->ptr[i] + carry;
        carry = (carry > ~a->ptr[i]);
    }
    size += carry;
    result->size = size;
    /* how to eliminate this branch */
    if (carry)
        result->ptr[size - 1] = carry;
}

/* this implementation assume a is greater than b */
static inline void bn_sub(bn *result, bn *a, bn *b)
{
    if (bn_capacity(result) < bn_size(a))
        bn_resize(result, bn_size(a));

    for (int i = 0, borrow = 0; i < bn_size(a); i++) {
        unsigned long long sub = bn_size(b) > i ? b->ptr[i] : 0;
        /* it's fine to overflow */
        result->ptr[i] = a->ptr[i] - sub - borrow;
        borrow = (a->ptr[i] < sub + borrow);
    }
    result->size = bn_size(a) - !(result->ptr[bn_size(a) - 1]);
}

static inline void bn_mul(bn *result, bn *a, bn *b)
{
    if (bn_capacity(result) < bn_size(a) + bn_size(b)) {
        bn_resize(result, bn_size(a) + bn_size(b));
        result->size = bn_size(a) + bn_size(b);
    }
}

/* it will dynamically resize */
static inline void bn_sll(bn *a, unsigned long long sha)
{
    /* quot = sha / N_BITS (bits) */
    unsigned long long quot = sha >> N_BITS_TZ;
    unsigned long long rem = sha & (N_BITS - 1);

    if (bn_capacity(a) < bn_size(a) + quot + 1)
        bn_resize(a, bn_size(a) + quot + 1);
    /* new size after shift */
    a->size += quot + (__builtin_clz(a->ptr[a->size - 1]) < rem);

    int i = bn_size(a) - 1;
    for (; i > quot; i--) {
        unsigned long long rhs_bits = a->ptr[i - quot - 1] >> (N_BITS - rem);
        a->ptr[i] = (a->ptr[i - quot] << rem) | rhs_bits;
    }

    a->ptr[i--] = a->ptr[0] << rem;
    for (; i >= 0; a->ptr[i--] = 0)
        ;
}

static inline void bn_srl(bn *a, unsigned long long sha)
{
    /* quot = sha / N_BITS (bits) */
    unsigned long long quot = sha >> N_BITS_TZ;
    unsigned long long rem = sha & (N_BITS - 1);
    const unsigned long long mask = (1 << rem) - 1;

    int i = 0;
    for (; i < bn_size(a) - quot - 1; i++) {
        /* performance can be improve here */
        unsigned long long lhs_bits = (a->ptr[i + quot + 1] & mask)
                                      << (N_BITS - rem);
        a->ptr[i] = lhs_bits | (a->ptr[i + quot] >> rem);
    }

    a->ptr[i++] = a->ptr[bn_size(a) - 1] >> rem;
    for (; i < bn_size(a); a->ptr[i++] = 0)
        ;
    /* new size after shift */
    a->size -= quot + (N_BITS - __builtin_clzll(a->ptr[a->size - 1]) <= rem);
}

static inline char *bn_hex(bn *a)
{
#define BUFSIZE 65536
#define DIGITS (N_BITS >> 2)  // hex digits per unsigned long long number
#define MASK 0xFull
    /* kzalloc - allocate memory. The memory is set to zero. */
    char *buf = kzalloc(BUFSIZE * sizeof(char), GFP_KERNEL);
    int idx = bn_size(a) * DIGITS;
    unsigned long long tem = a->ptr[bn_size(a) - 1];

    /* for zero case */
    if (!tem) {
        buf[0] = '0';
        goto out;
    }

    /* find first non-zero hex digit */
    for (; !(tem & (MASK << (N_BITS - 4))); tem <<= 4, idx--)
        ;
    buf[idx] = '\0';

    for (int i = 0; i < bn_size(a); i++) {
        tem = a->ptr[i];
        for (int j = 0; idx && (j < DIGITS); j++, tem >>= 4)
            buf[--idx] = "0123456789ABCDEF"[tem & MASK];
    }
out:
    return buf;
#undef MASK
#undef DIGITS
#undef BUFSIZE
}
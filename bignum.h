#include <linux/slab.h>

typedef struct bignum {
    unsigned long long *ptr;
    unsigned int size;  //#sizeof(unsigned long long) bytes are used
    unsigned int capacity;
} bn;

/* number of bits of unsigned long long type */
#define BITS sizeof(unsigned long long) * 8
/* log_2(BITS) */
#define BITS_TZ __builtin_ctz(BITS)

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


#define bn_swap(a, b)      \
    do {                   \
        typeof(*a) t = *a; \
        *a = *b;           \
        *b = t;            \
    } while (0)

/* retrun non-zero if a is greater than b */
static inline int bn_greater(bn *a, bn *b)
{
#define SIGN_BIT 1ull << (BITS - 1)
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
    if (bn_size(a) < bn_size(b))
        bn_swap(&a, &b);
    unsigned int size = bn_size(a);

    /* prevent potential overflow */
    if (bn_capacity(result) < size + 1)
        bn_resize(result, bn_size(a) + 1);

    int i, carry = 0;
    for (i = 0; i < bn_size(b); i++) {
        unsigned long long a_val = a->ptr[i];
        unsigned long long b_val = b->ptr[i];
        carry = (a_val += carry) < carry;
        carry += (result->ptr[i] = a_val + b_val) < b_val;
    }
    for (; i < size; i++) {
        unsigned long long a_val = a->ptr[i];
        result->ptr[i] = a_val + carry;
        carry = (carry > ~a_val);
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
        unsigned long long a_val = a->ptr[i];
        unsigned long long sub = bn_size(b) > i ? b->ptr[i] : 0;
        /* it's fine to overflow */
        result->ptr[i] = a_val - sub - borrow;
        borrow = (a_val < sub + borrow);
    }
    result->size = bn_size(a) - !(result->ptr[bn_size(a) - 1]);
}

static inline void bn_sll(bn *result, bn *a, unsigned long long sha);
static inline void bn_sll(bn *result, bn *a, unsigned long long sha);
static inline void bn_mul(bn *result, bn *a, bn *b)
{
    bn r = *result;
    unsigned long long *be_free = NULL;

    if (result == a || result == b) {
        bn_init(&r);
        be_free = result->ptr;
    }

    if (bn_capacity(&r) < bn_size(a) + bn_size(b))
        bn_resize(&r, bn_size(a) + bn_size(b));
    bn tem;
    bn_init(&tem);
    bn_assign(&r, 0);

    unsigned long long check_bit = 1;
    for (int i = 0; i < bn_size(a); i++, check_bit = 1)
        for (int j = 0; j < BITS; j++, check_bit <<= 1)
            if (check_bit & a->ptr[i]) {
                bn_sll(&tem, b, (i << BITS_TZ) + j);
                bn_add(&r, &r, &tem);
            }

    *result = r;
    kfree(be_free);
    kfree(tem.ptr);
}

/* it will dynamically resize */
static inline void bn_sll(bn *result, bn *a, unsigned long long sha)
{
    /* quot = sha / BITS (bits) */
    unsigned long long quot = sha >> BITS_TZ;
    unsigned long long rem = sha & (BITS - 1);

    if (bn_capacity(result) < bn_size(a) + quot + 1)
        bn_resize(result, bn_size(a) + quot + 1);
    /* new size after shift */
    result->size =
        bn_size(a) + quot + (__builtin_clzll(a->ptr[a->size - 1]) < rem);

    int i = bn_size(result) - 1;
    for (; i > quot; i--) {
        /* performance can be improve here */
        unsigned long long rhs_bits =
            rem ? a->ptr[i - quot - 1] >> (BITS - rem) : 0;
        result->ptr[i] = (a->ptr[i - quot] << rem) | rhs_bits;
    }

    result->ptr[i--] = a->ptr[0] << rem;
    for (; i >= 0; result->ptr[i--] = 0)
        ;
}

static inline void bn_srl(bn *result, bn *a, unsigned long long sha)
{
    /* quot = sha / BITS (bits) */
    unsigned long long quot = sha >> BITS_TZ;
    unsigned long long rem = sha & (BITS - 1);
    const unsigned long long mask = (1 << rem) - 1;
    unsigned long long newsize =
        bn_size(a) - quot - (rem >= __builtin_clzll(a->ptr[bn_size(a) - 1]));

    if (bn_capacity(result) < newsize)
        bn_resize(result, newsize);

    result->size = newsize;

    int i = 0;
    for (; i < bn_size(a) - quot - 1; i++) {
        /* performance can be improve here */
        unsigned long long lhs_bits =
            rem ? (a->ptr[i + quot + 1] & mask) << (BITS - rem) : 0;
        result->ptr[i] = lhs_bits | (a->ptr[i + quot] >> rem);
    }

    result->ptr[i++] = a->ptr[bn_size(a) - 1] >> rem;
    for (; i < newsize; result->ptr[i++] = 0)
        ;
}

static inline void bn_free(bn *a)
{
    kfree(a->ptr);
    a->ptr = NULL;
}

static inline char *bn_hex(bn *a)
{
#define BUFSIZE 65536
#define DIGITS (BITS >> 2)  // hex digits per unsigned long long number
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
    for (; !(tem & (MASK << (BITS - 4))); tem <<= 4, idx--)
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
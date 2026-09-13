/*
 * Expanding OpSDot for cards without a hardware dot product.
 *
 * FSR4 decomposes its int8 multiply-accumulates by hand, and dxil-spirv fuses that back into
 * OpSDot, the packed 4x8 integer dot product. GCN4 has no such instruction, so the driver lowers it,
 * and the result is slower than doing the four multiplies directly in 32 bits. This rewrites
 *
 *     r = OpSDot(a, b)
 *
 * into four sign-extending byte extracts per operand, four multiplies and three adds, all 32-bit.
 * NIR then recognises the pattern and ACO issues one `v_mad_i32_i24` per multiply.
 *
 * The rewrite is exact: a signed 4x8 dot product is the sum of the four signed byte products.
 * It only runs when the device reports that the packed signed dot product is not accelerated.
 */

#define SPV_MAGIC 0x07230203u
#define SPV_OP_TYPE_INT 21
#define SPV_OP_CONSTANT 43
#define SPV_OP_FUNCTION 54
#define SPV_OP_IADD 128
#define SPV_OP_IMUL 132
#define SPV_OP_BITFIELD_SEXTRACT 202
#define SPV_OP_SDOT 4450

struct spv_writer {
    uint32_t *w;
    size_t n, cap;
};

static void spv_put(struct spv_writer *o, uint32_t v)
{
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 4096;
        o->w = realloc(o->w, o->cap * sizeof(uint32_t));
    }
    o->w[o->n++] = v;
}

static void spv_emit(struct spv_writer *o, uint16_t opcode, const uint32_t *ops, unsigned count)
{
    spv_put(o, ((uint32_t)(count + 1) << 16) | opcode);
    for (unsigned i = 0; i < count; i++)
        spv_put(o, ops[i]);
}

/*
 * Returns a rewritten module, or NULL when there is nothing to do. The caller frees it.
 */
static uint32_t *spv_expand_sdot(const uint32_t *code, size_t size, size_t *out_size)
{
    if (size < 20 || code[0] != SPV_MAGIC)
        return NULL;

    const size_t words = size / 4;
    uint32_t bound = code[3];

    /* First pass: is there anything to expand, which integer type to use, and which constants exist. */
    uint32_t int_type = 0;
    uint32_t konst[4] = { 0, 0, 0, 0 };          /* values 0, 8, 16, 24 */
    static const uint32_t want[4] = { 0, 8, 16, 24 };
    unsigned sdots = 0;
    size_t first_function = 0;

    for (size_t i = 5; i < words;) {
        uint32_t word = code[i];
        unsigned len = word >> 16;
        uint16_t op = word & 0xffff;
        if (len == 0 || i + len > words)
            return NULL;                         /* malformed, leave it alone */

        if (op == SPV_OP_TYPE_INT && len >= 4 && code[i + 2] == 32 && !int_type)
            int_type = code[i + 1];
        else if (op == SPV_OP_CONSTANT && len == 4) {
            for (unsigned k = 0; k < 4; k++)
                if (!konst[k] && code[i + 3] == want[k] && code[i + 1] == int_type)
                    konst[k] = code[i + 2];
        } else if (op == SPV_OP_SDOT)
            sdots++;
        else if (op == SPV_OP_FUNCTION && !first_function)
            first_function = i;

        i += len;
    }

    if (!sdots || !int_type || !first_function)
        return NULL;

    /* Second pass: copy, adding any missing constants, and expanding each dot product. */
    struct spv_writer o = { 0 };
    for (unsigned k = 0; k < 5; k++)
        spv_put(&o, code[k]);

    for (size_t i = 5; i < words;) {
        uint32_t word = code[i];
        unsigned len = word >> 16;
        uint16_t op = word & 0xffff;

        if (i == first_function) {
            for (unsigned k = 0; k < 4; k++)
                if (!konst[k]) {
                    konst[k] = bound++;
                    uint32_t ops[3] = { int_type, konst[k], want[k] };
                    spv_emit(&o, SPV_OP_CONSTANT, ops, 3);
                }
        }

        if (op == SPV_OP_SDOT && len >= 5) {
            uint32_t type = code[i + 1], result = code[i + 2];
            uint32_t a = code[i + 3], b = code[i + 4];
            uint32_t product[4];

            for (unsigned k = 0; k < 4; k++) {
                uint32_t ea = bound++, eb = bound++;
                /* offset = 8k, count = 8, which is konst[1] */
                uint32_t ex[5] = { type, ea, a, konst[k], konst[1] };
                spv_emit(&o, SPV_OP_BITFIELD_SEXTRACT, ex, 5);
                ex[1] = eb;
                ex[2] = b;
                spv_emit(&o, SPV_OP_BITFIELD_SEXTRACT, ex, 5);
                product[k] = bound++;
                uint32_t mul[4] = { type, product[k], ea, eb };
                spv_emit(&o, SPV_OP_IMUL, mul, 4);
            }

            uint32_t sum = bound++;
            uint32_t add[4] = { type, sum, product[0], product[1] };
            spv_emit(&o, SPV_OP_IADD, add, 4);
            uint32_t sum2 = bound++;
            add[1] = sum2; add[2] = sum; add[3] = product[2];
            spv_emit(&o, SPV_OP_IADD, add, 4);
            /* The last add keeps the original result id, so everything downstream still works. */
            add[1] = result; add[2] = sum2; add[3] = product[3];
            spv_emit(&o, SPV_OP_IADD, add, 4);

            i += len;
            continue;
        }

        for (unsigned k = 0; k < len; k++)
            spv_put(&o, code[i + k]);
        i += len;
    }

    o.w[3] = bound;
    *out_size = o.n * sizeof(uint32_t);
    return o.w;
}

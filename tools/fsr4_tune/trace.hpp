// Tracing interpreter for dxil-spirv GLSL compute shaders.
// Runs the shader body for one invocation with concrete values. Values that depend on the tensor
// (binding 11) become SSA nodes; everything else folds to constants. Weights (binding 18) are
// constants, and a multiply by a weight byte with |w| <= g_prune_thr folds to 0.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

enum Ty { TU, TI, TB, TF, TF16, TI16, TU16, TU8, TI8 };
static const char *ty_name[] = { "uint", "int", "bool", "float", "float16_t", "int16_t", "uint16_t", "uint8_t", "int8_t" };

struct Node { Ty ty; std::string expr; };
struct LoadRec { int node; int64_t idx; };
struct StoreRec { int64_t idx; int node; int64_t cval; };

std::vector<Node> g_nodes;
std::unordered_map<std::string, int> g_cse;
std::vector<LoadRec> g_loads;
std::vector<StoreRec> g_stores;
std::vector<uint32_t> g_weights;          // binding 18, one dword per index
std::vector<int> g_wbytes;                // weight bytes seen in MACs (for quantiles)
int g_prune_thr = -1;
long g_macs = 0, g_pruned = 0;
// Scale applied to surviving weights, so that the layer keeps the magnitude that pruning removed.
// 1.0 disables it.
double g_wscale = 1.0;

[[noreturn]] static void die(const char *m) { fprintf(stderr, "trace error: %s\n", m); exit(3); }

static bool is_int(Ty t) { return t != TF && t != TF16; }
static int64_t norm(Ty t, int64_t v) {
    switch (t) {
    case TU: return (uint32_t)v; case TI: return (int32_t)v; case TB: return v != 0;
    case TI16: return (int16_t)v; case TU16: return (uint16_t)v; case TU8: return (uint8_t)v; case TI8: return (int8_t)v;
    default: return v;
    }
}
static double half_round(double f) {
    if (!std::isfinite(f) || f == 0) return f;
    int e; std::frexp(f, &e);                     // f = m * 2^e, 0.5 <= |m| < 1
    // float16 has 11 significant bits for normal values and a fixed 2^-24 grid for subnormals.
    int step = e - 11 > -24 ? e - 11 : -24;
    double q = std::nearbyint(std::ldexp(f, -step));
    double r = std::ldexp(q, step);
    return std::fabs(r) > 65504.0 ? std::copysign(INFINITY, r) : r;
}

struct V {
    Ty ty = TU; int64_t i = 0; double f = 0; int n = -1; bool w = false; bool poison = false;
    V() {}
    V(unsigned v) : ty(TU), i(v) {}
    V(int v) : ty(TI), i(v) {}
    V(double v) : ty(TF), f(v) {}
    V(bool v) : ty(TB), i(v) {}
    bool c() const { return n < 0; }
    std::string s() const {
        char b[64];
        if (n >= 0) { snprintf(b, sizeof b, "n%d", n); return b; }
        switch (ty) {
        case TU: snprintf(b, sizeof b, "%lluu", (unsigned long long)(uint32_t)i); return b;
        case TI: snprintf(b, sizeof b, "int(%lld)", (long long)i); return b;
        case TB: return i ? "true" : "false";
        case TF: case TF16: {
            if (std::isinf(f)) return f > 0 ? "(1.0/0.0)" : "(-1.0/0.0)";
            snprintf(b, sizeof b, "%.17g", f); std::string r = b;
            if (r.find_first_of(".en") == std::string::npos) r += ".0";
            return ty == TF ? r : "float16_t(" + r + ")";
        }
        default: snprintf(b, sizeof b, "%s(%lld)", ty_name[ty], (long long)i); return b;
        }
    }
    explicit operator bool() const { if (!c()) die("data-dependent branch"); return i != 0; }
};

static V mk(Ty t, int64_t iv, double fv, const std::string &e) {
    V r; r.ty = t; r.i = norm(t, iv); r.f = fv;
    std::string key = std::string(ty_name[t]) + ":" + e;
    auto it = g_cse.find(key);
    if (it != g_cse.end()) { r.n = it->second; return r; }
    r.n = (int)g_nodes.size(); g_nodes.push_back({ t, e }); g_cse[key] = r.n;
    return r;
}
static V cst(Ty t, int64_t iv, double fv) { V r; r.ty = t; r.i = norm(t, iv); r.f = (t == TF16) ? half_round(fv) : fv; return r; }

// ---- conversions (GLSL constructor-style casts)
static V conv(Ty t, const V &a) {
    if (a.poison && a.ty != t) die("uniform z/w component used");
    if (a.ty == t) return a;
    int64_t iv = 0; double fv = 0;
    if (is_int(t)) {
        if (is_int(a.ty)) iv = a.i;
        else { if (!std::isfinite(a.f)) die("inf to int"); iv = (int64_t)std::trunc(a.f); }
        if (t == TB) iv = is_int(a.ty) ? a.i != 0 : a.f != 0;
    } else {
        fv = is_int(a.ty) ? (double)a.i : a.f;
        if (a.ty == TU && !is_int(t)) fv = (double)(uint32_t)a.i;
    }
    if (a.c()) { V r = cst(t, iv, fv); r.w = a.w; return r; }
    return mk(t, iv, t == TF16 ? half_round(fv) : fv, std::string(ty_name[t]) + "(" + a.s() + ")");
}

#define SCALAR(NAME, T)                                                        \
    struct NAME : V {                                                          \
        NAME() { ty = T; }                                                     \
        NAME(const V &a) : V(conv(T, a)) {}                                    \
        NAME(unsigned v) : V(conv(T, V(v))) {}                                 \
        NAME(int v) : V(conv(T, V(v))) {}                                      \
        NAME(double v) : V(conv(T, V(v))) {}                                   \
        NAME(bool v) : V(conv(T, V(v))) {}                                     \
    };
SCALAR(G_uint, TU) SCALAR(G_int, TI) SCALAR(G_bool, TB) SCALAR(G_float, TF) SCALAR(G_float16_t, TF16)
SCALAR(G_int16_t, TI16) SCALAR(G_uint16_t, TU16) SCALAR(G_uint8_t, TU8) SCALAR(G_int8_t, TI8)

// ---- arithmetic
static std::string pe(const V &a) { return a.n >= 0 ? a.s() : "(" + a.s() + ")"; }

static V arith(const V &a0, const V &b0, const char *op);
static V arith(const V &a0, const V &b0, const char *op) {
    if (a0.poison || b0.poison) die("uniform z/w component used");
    V a = a0, b = b0;
    bool shift = !strcmp(op, "<<") || !strcmp(op, ">>");
    if (!shift && a.ty != b.ty) {
        // Literal operands adopt the other side's type.
        if (a.c() && !b.c()) a = conv(b.ty, a); else if (b.c() && !a.c()) b = conv(a.ty, b);
        else if (a.c() && b.c()) b = conv(a.ty, b);
        else die("mixed types");
    }
    Ty t = a.ty;
    bool wa = a.c() && a.w, wb = b.c() && b.w;
    if (!strcmp(op, "*") && (wa || wb) && (a.c() != b.c())) {
        const V &k = wa ? a : b;
        g_macs++;
        if (g_prune_thr < 0) g_wbytes.push_back((int)(int8_t)(uint8_t)k.i);
        if (std::llabs((int64_t)(int8_t)(uint8_t)k.i) <= g_prune_thr) { g_pruned++; return cst(t, 0, 0); }
        if (g_wscale != 1.0) {
            // Rebuild the multiply with the scaled weight. It stays well inside 24 bits, so the
            // 24-bit multiply-add path still applies.
            int64_t w = (int64_t)std::llround((double)(int8_t)(uint8_t)k.i * g_wscale);
            V ks = cst(t, w, 0);
            return arith(wa ? ks : a, wa ? b : ks, "*");
        }
    }
    if (a.c() && b.c()) {
        int64_t x = a.i, y = b.i; double fx = a.f, fy = b.f; int64_t r = 0; double fr = 0;
        if (is_int(t)) {
            bool u = (t == TU || t == TU16 || t == TU8);
            uint64_t ux = (uint64_t)x & 0xffffffffull, uy = (uint64_t)y & 0xffffffffull;
            if (!strcmp(op, "+")) r = x + y; else if (!strcmp(op, "-")) r = x - y; else if (!strcmp(op, "*")) r = u ? (int64_t)(ux * uy) : x * y;
            else if (!strcmp(op, "/")) { if (!y) die("div0"); r = u ? (int64_t)(ux / uy) : x / y; }
            else if (!strcmp(op, "%")) { if (!y) die("mod0"); r = u ? (int64_t)(ux % uy) : x % y; }
            else if (!strcmp(op, "&")) r = x & y; else if (!strcmp(op, "|")) r = x | y; else if (!strcmp(op, "^")) r = x ^ y;
            else if (!strcmp(op, "<<")) r = (int64_t)((uint64_t)x << (y & 31));
            else if (!strcmp(op, ">>")) r = u ? (int64_t)(ux >> (y & 31)) : (int64_t)((int32_t)x >> (y & 31));
            else die(op);
        } else {
            if (!strcmp(op, "+")) fr = fx + fy; else if (!strcmp(op, "-")) fr = fx - fy; else if (!strcmp(op, "*")) fr = fx * fy;
            else if (!strcmp(op, "/")) fr = fx / fy; else die(op);
            if (t == TF) fr = (float)fr; else fr = half_round(fr);
        }
        V k = cst(t, r, fr); k.w = a.w || b.w; return k;
    }
    // Identity folds.
    auto zero = [](const V &v) { return v.c() && (is_int(v.ty) ? v.i == 0 : v.f == 0); };
    auto one = [](const V &v) { return v.c() && (is_int(v.ty) ? v.i == 1 : v.f == 1); };
    if (!strcmp(op, "+") || !strcmp(op, "|") || !strcmp(op, "^")) { if (zero(a)) return b; if (zero(b)) return a; }
    if (!strcmp(op, "-") || shift) { if (zero(b)) return a; }
    if (!strcmp(op, "*")) { if (is_int(t) && (zero(a) || zero(b))) return cst(t, 0, 0); if (one(a)) return b; if (one(b)) return a; }
    if (!strcmp(op, "&") && (zero(a) || zero(b))) return cst(t, 0, 0);
    return mk(t, 0, 0, pe(a) + " " + op + " " + pe(b));
}
#define OP(o, s) static V operator o(const V &a, const V &b) { return arith(a, b, s); }
OP(+, "+") OP(-, "-") OP(*, "*") OP(/, "/") OP(%, "%") OP(&, "&") OP(|, "|") OP(^, "^") OP(<<, "<<") OP(>>, ">>")
#define OPL(o, s) static V operator o(const V &a, unsigned b) { return arith(a, V(b), s); } static V operator o(unsigned a, const V &b) { return arith(V(a), b, s); }
OPL(+, "+") OPL(-, "-") OPL(*, "*") OPL(/, "/") OPL(%, "%") OPL(&, "&") OPL(|, "|") OPL(^, "^") OPL(<<, "<<") OPL(>>, ">>")
#define OPD(o, s) static V operator o(const V &a, double b) { return arith(a, V(b), s); } static V operator o(double a, const V &b) { return arith(V(a), b, s); }
OPD(+, "+") OPD(-, "-") OPD(*, "*") OPD(/, "/")
static V operator-(const V &a) { return arith(cst(a.ty, 0, 0), a, "-"); }

static V cmp(const V &a0, const V &b0, const char *op) {
    if (a0.poison || b0.poison) die("uniform z/w component used");
    V a = a0, b = b0;
    if (a.ty != b.ty) { if (a.c()) a = conv(b.ty, a); else b = conv(a.ty, b); }
    if (a.c() && b.c()) {
        bool r;
        if (is_int(a.ty)) {
            bool u = a.ty == TU || a.ty == TU16 || a.ty == TU8;
            uint64_t x = (uint64_t)a.i & 0xffffffffull, y = (uint64_t)b.i & 0xffffffffull; int64_t sx = a.i, sy = b.i;
            if (!strcmp(op, "<")) r = u ? x < y : sx < sy; else if (!strcmp(op, "<=")) r = u ? x <= y : sx <= sy;
            else if (!strcmp(op, ">")) r = u ? x > y : sx > sy; else if (!strcmp(op, ">=")) r = u ? x >= y : sx >= sy;
            else if (!strcmp(op, "==")) r = a.i == b.i; else r = a.i != b.i;
        } else {
            if (!strcmp(op, "<")) r = a.f < b.f; else if (!strcmp(op, "<=")) r = a.f <= b.f; else if (!strcmp(op, ">")) r = a.f > b.f;
            else if (!strcmp(op, ">=")) r = a.f >= b.f; else if (!strcmp(op, "==")) r = a.f == b.f; else r = a.f != b.f;
        }
        return cst(TB, r, 0);
    }
    return mk(TB, 0, 0, pe(a) + " " + op + " " + pe(b));
}
#define CMP(o, s) static V operator o(const V &a, const V &b) { return cmp(a, b, s); } \
    static V operator o(const V &a, unsigned b) { return cmp(a, V(b), s); } static V operator o(const V &a, double b) { return cmp(a, V(b), s); }
CMP(<, "<") CMP(<=, "<=") CMP(>, ">") CMP(>=, ">=") CMP(==, "==") CMP(!=, "!=")
static V operator&&(const V &a, const V &b) { if (a.c() && !a.i) return a; if (b.c() && !b.i) return b; if (a.c()) return b; if (b.c()) return a; return mk(TB, 0, 0, pe(a) + " && " + pe(b)); }
static V operator||(const V &a, const V &b) { if (a.c() && a.i) return a; if (b.c() && b.i) return b; if (a.c()) return b; if (b.c()) return a; return mk(TB, 0, 0, pe(a) + " || " + pe(b)); }
static V operator!(const V &a) { if (a.c()) return cst(TB, !a.i, 0); return mk(TB, 0, 0, "!" + pe(a)); }
static V& operator++(V &a) { a = a + 1u; return a; }
static V operator++(V &a, int) { V o = a; a = a + 1u; return o; }

// ---- vectors
#define VEC(NAME, T)                                                                              \
    struct NAME {                                                                                 \
        V x, y, z, w;                                                                             \
        NAME() : x(cst(T, 0, 0)), y(x), z(x), w(x) {}                                            \
        NAME(const V &a) : x(conv(T, a)), y(x), z(x), w(x) {}                                    \
        NAME(const V &a, const V &b) : x(conv(T, a)), y(conv(T, b)), z(cst(T, 0, 0)), w(z) {}    \
        NAME(const V &a, const V &b, const V &c, const V &d) : x(conv(T, a)), y(conv(T, b)), z(conv(T, c)), w(conv(T, d)) {} \
        template <class O> NAME(const O &o, decltype(o.w) * = nullptr) : x(conv(T, o.x)), y(conv(T, o.y)), z(conv(T, o.z)), w(conv(T, o.w)) {} \
    };
VEC(G_uvec4, TU) VEC(G_ivec4, TI) VEC(G_u8vec4, TU8) VEC(G_i8vec4, TI8) VEC(G_uvec2, TU) VEC(G_vec4, TF)
VEC(G_f16vec4, TF16) VEC(G_f16vec2, TF16) VEC(G_vec2, TF) VEC(G_uvec3, TU) VEC(G_i16vec4, TI16) VEC(G_u16vec4, TU16)

static V fmax(const V &a0, const V &b0) {
    V a = a0, b = b0; if (a.ty != b.ty) { if (a.c()) a = conv(b.ty, a); else b = conv(a.ty, b); }
    if (a.c() && b.c()) return is_int(a.ty) ? cst(a.ty, (a.ty == TU ? ((uint32_t)a.i > (uint32_t)b.i) : a.i > b.i) ? a.i : b.i, 0) : cst(a.ty, 0, std::max(a.f, b.f));
    return mk(a.ty, 0, 0, "max(" + a.s() + ", " + b.s() + ")");
}
static V fmin(const V &a0, const V &b0) {
    V a = a0, b = b0; if (a.ty != b.ty) { if (a.c()) a = conv(b.ty, a); else b = conv(a.ty, b); }
    if (a.c() && b.c()) return is_int(a.ty) ? cst(a.ty, (a.ty == TU ? ((uint32_t)a.i < (uint32_t)b.i) : a.i < b.i) ? a.i : b.i, 0) : cst(a.ty, 0, std::min(a.f, b.f));
    return mk(a.ty, 0, 0, "min(" + a.s() + ", " + b.s() + ")");
}
static V max(const V &a, const V &b) { return fmax(a, b); }
static V min(const V &a, const V &b) { return fmin(a, b); }
static V clamp(const V &v, const V &lo, const V &hi) { return fmin(fmax(v, lo), hi); }
static G_ivec4 clamp(const G_ivec4 &v, const G_ivec4 &lo, const G_ivec4 &hi) { G_ivec4 r; r.x = clamp(v.x, lo.x, hi.x); r.y = clamp(v.y, lo.y, hi.y); r.z = clamp(v.z, lo.z, hi.z); r.w = clamp(v.w, lo.w, hi.w); return r; }
static V spvNMax(const V &a, const V &b) { return fmax(a, b); }
static V spvNMin(const V &a, const V &b) { return fmin(a, b); }
static V roundEven(const V &a) { if (a.c()) return cst(a.ty, 0, std::nearbyint(a.f)); return mk(a.ty, 0, 0, "roundEven(" + a.s() + ")"); }
static V abs(const V &a) { if (a.c()) return is_int(a.ty) ? cst(a.ty, std::llabs(a.i), 0) : cst(a.ty, 0, std::fabs(a.f)); return mk(a.ty, 0, 0, "abs(" + a.s() + ")"); }
static V bitfieldExtract(const V &v, const V &off, const V &bits) {
    if (!off.c() || !bits.c()) die("bitfieldExtract with data offset");
    int o = (int)off.i, nb = (int)bits.i;
    // An 8-bit field of a constant is a weight byte: some passes stream weights from binding 18,
    // others bake them into the shader as dword literals.
    if (v.c()) { int64_t x = ((uint64_t)(uint32_t)v.i >> o) & ((1ull << nb) - 1); if (x >> (nb - 1)) x -= (1ll << nb); V r = cst(TI, x, 0); r.w = v.w || nb == 8; return r; }
    char b[64]; snprintf(b, sizeof b, ", %d, %d)", o, nb);
    return mk(TI, 0, 0, "bitfieldExtract(int(" + v.s() + ")" + b);
}
static V ByteAddressMask(const V &index, const V &stride) { return index & (4294967295u / stride); }
static V uintBitsToFloat(const V &a) { if (a.c()) { uint32_t u = (uint32_t)a.i; float f; memcpy(&f, &u, 4); return cst(TF, 0, f); } return mk(TF, 0, 0, "uintBitsToFloat(" + a.s() + ")"); }
// Uniform reads are live tensor sizes, used only as `id < size` guards. They trace as a large
// constant, and main.cpp emits the guard in the flat shader.
// Each entry is the axis ('x' or 'y') followed by the GLSL expression of the size it is compared to.
std::vector<std::string> g_ubo_used;
static V size_guard(char axis, const std::string &expr) {
    if (axis == 'z' || axis == 'w') { V p = cst(TU, 0x7fffffff, 0); p.poison = true; return p; }
    std::string key = std::string(1, axis) + expr;
    bool seen = false; for (auto &s : g_ubo_used) seen |= s == key;
    if (!seen) g_ubo_used.push_back(key);
    return cst(TU, 0x7fffffff, 0);
}
static V floatBitsToUint(const V &a) {
    if (!a.c() && g_nodes[a.n].expr.find("._m0[") != std::string::npos) {
        const std::string &e = g_nodes[a.n].expr;
        return size_guard(e.back(), "floatBitsToUint(" + e + ")");
    }
    if (a.c()) { float f = (float)a.f; uint32_t u; memcpy(&u, &f, 4); return cst(TU, u, 0); } return mk(TU, 0, 0, "floatBitsToUint(" + a.s() + ")"); }
static G_uvec4 floatBitsToUint(const G_vec4 &a) { return G_uvec4(floatBitsToUint(a.x), floatBitsToUint(a.y), floatBitsToUint(a.z), floatBitsToUint(a.w)); }
static V pack32(const G_u8vec4 &v) {
    if (v.x.c() && v.y.c() && v.z.c() && v.w.c()) return cst(TU, (uint32_t)v.x.i | ((uint32_t)v.y.i << 8) | ((uint32_t)v.z.i << 16) | ((uint32_t)v.w.i << 24), 0);
    return mk(TU, 0, 0, "pack32(u8vec4(" + v.x.s() + ", " + v.y.s() + ", " + v.z.s() + ", " + v.w.s() + "))");
}
static G_u8vec4 unpack8(const V &u) {
    G_u8vec4 r;
    r.x = conv(TU8, u & 255u); r.y = conv(TU8, (u >> 8u) & 255u); r.z = conv(TU8, (u >> 16u) & 255u); r.w = conv(TU8, (u >> 24u) & 255u);
    return r;
}

// ---- resources
struct TensorBuf {} T_;
struct WeightBuf {} W_;
static G_uvec4 texelFetch(WeightBuf, const V &idx, int = 0) {
    if (!idx.c()) die("weight index depends on data");
    uint64_t k = (uint32_t)idx.i;
    if (k >= g_weights.size()) die("weight index out of range");
    V r = cst(TU, g_weights[k], 0); r.w = true;
    G_uvec4 o; o.x = r; return o;
}
static G_uvec4 imageLoad(TensorBuf, const V &idx) {
    if (!idx.c()) die("tensor index depends on data");
    for (const LoadRec &l : g_loads)
        if (l.idx == (int64_t)(uint32_t)idx.i) { V r; r.ty = TU; r.n = l.node; G_uvec4 o; o.x = r; return o; }
    char b[32]; snprintf(b, sizeof b, "@L%zu", g_loads.size());
    V r = mk(TU, 0, 0, b);
    g_loads.push_back({ r.n, (int64_t)(uint32_t)idx.i });
    G_uvec4 o; o.x = r; return o;
}
static void imageStore(TensorBuf, const V &idx, const G_uvec4 &v) {
    if (!idx.c()) die("store index depends on data");
    g_stores.push_back({ (int64_t)(uint32_t)idx.i, v.x.n, v.x.c() ? (int64_t)(uint32_t)v.x.i : 0 });
}

struct { V x, y, z; } gl_GlobalInvocationID, gl_WorkGroupID, gl_LocalInvocationID;

// ---- vkd3d-proton form: descriptor-array SSBO accesses and buffer-reference constant buffers
static V TL(const V &idx) { return imageLoad(T_, idx).x; }
static G_uvec4 TL4(const V &idx) { V b = idx * 4u; return G_uvec4(TL(b), TL(b + 1u), TL(b + 2u), TL(b + 3u)); }
static void TS(const V &idx, const V &v) { imageStore(T_, idx, G_uvec4(v)); }
static V WF(const V &idx) { return texelFetch(W_, idx).x; }
static G_uvec4 WF4(const V &idx) { V b = idx * 4u; return G_uvec4(WF(b), WF(b + 1u), WF(b + 2u), WF(b + 3u)); }
struct CBVArr {
    std::string sp;
    G_uvec4 operator[](size_t k) const {
        char b[32]; snprintf(b, sizeof b, ".value[%zuu].", k);
        std::string base = sp + b;
        return G_uvec4(size_guard('x', base + "x"), size_guard('y', base + "y"), size_guard('z', base + "z"), size_guard('w', base + "w"));
    }
};
struct CBV { CBVArr value; CBV(const char *sp) { value.sp = sp; } };
struct { V _m0, _m1, _m2, _m3; } registers;

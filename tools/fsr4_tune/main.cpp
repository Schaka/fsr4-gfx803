// Trace one FSR4 pass and emit a flat GLSL shader with baked, optionally pruned, weights.
// Usage: tracer weights.bin prune(-1 | N | qFRAC) orig.glsl meta.txt out.glsl
#include "trace.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

static size_t IX(const V &v) { if (!v.c()) die("data-dependent array index"); return (uint32_t)v.i; }
static size_t IX(unsigned v) { return v; }
static size_t IX(int v) { return (size_t)v; }
// Uniform buffer values stay run-time inputs in the emitted shader.
std::string g_ubo_name = "_18";
struct UboArr {
    G_vec4 operator[](size_t k) const {
        G_vec4 v; V *comp[4] = { &v.x, &v.y, &v.z, &v.w };
        for (int i = 0; i < 4; i++) {
            char b[64]; snprintf(b, sizeof b, "%s._m0[%zuu].%c", g_ubo_name.c_str(), k, "xyzw"[i]);
            *comp[i] = mk(TF, 0, 0, b);
        }
        return v;
    }
};
struct { UboArr _m0; } U_;

#include "body.inc"

struct Trace { std::vector<Node> nodes; std::vector<LoadRec> loads; std::vector<StoreRec> stores; long macs, pruned; };

static Trace run(uint32_t gx, uint32_t gy, uint32_t lsx) {
    g_nodes.clear(); g_cse.clear(); g_loads.clear(); g_stores.clear(); g_ubo_used.clear(); g_macs = g_pruned = 0;
    gl_GlobalInvocationID.x = V(gx); gl_GlobalInvocationID.y = V(gy); gl_GlobalInvocationID.z = V(0u);
    gl_WorkGroupID.x = V(gx / lsx); gl_WorkGroupID.y = V(gy); gl_WorkGroupID.z = V(0u);
    gl_LocalInvocationID.x = V(gx % lsx); gl_LocalInvocationID.y = V(0u); gl_LocalInvocationID.z = V(0u);
    shader_main();
    return { g_nodes, g_loads, g_stores, g_macs, g_pruned };
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage\n"); return 1; }
    {
        std::ifstream f(argv[1], std::ios::binary);
        std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
        g_weights.resize(b.size() / 4);
        memcpy(g_weights.data(), b.data(), g_weights.size() * 4);
    }
    std::string tname; unsigned lsx, bx, by;
    { std::ifstream m(argv[4]); m >> tname >> lsx >> bx >> by >> g_ubo_name; }
    uint32_t x0 = 5, y0 = 5;

    std::string pr = argv[2];
    if (pr[0] == 'q') {
        g_prune_thr = -1; g_wbytes.clear();
        run(x0, y0, lsx);
        std::vector<int> a; for (int w : g_wbytes) a.push_back(std::abs(w));
        std::sort(a.begin(), a.end());
        double q = atof(pr.c_str() + 1);
        g_prune_thr = q <= 0 ? -1 : a[std::min(a.size() - 1, (size_t)(q * a.size()))];
        // Strict quantile: prune values <= thr, so step down when too many fall at the threshold.
        size_t le = std::upper_bound(a.begin(), a.end(), g_prune_thr) - a.begin();
        if (q > 0 && (double)le / a.size() > q + 0.02) g_prune_thr--;
        fprintf(stderr, "weight bytes in MACs: %zu, |w| quantile %.2f -> threshold %d\n", a.size(), q, g_prune_thr);
    } else {
        g_prune_thr = atoi(pr.c_str());
    }

    // WSCALE=1 keeps the summed weight magnitude of the pass: the surviving weights take over the
    // share that the pruned ones carried.
    if (getenv("WSCALE") && g_prune_thr > 0) {
        int keep = g_prune_thr;
        g_prune_thr = -1; g_wbytes.clear();
        run(x0, y0, lsx);
        double all = 0, kept = 0;
        for (int w : g_wbytes) { all += std::abs(w); if (std::abs(w) > keep) kept += std::abs(w); }
        g_prune_thr = keep;
        g_wscale = kept > 0 ? all / kept : 1.0;
        fprintf(stderr, "weight magnitude kept %.3f, scaling surviving weights by %.3f\n", kept / all, g_wscale);
    }

    Trace t0 = run(x0, y0, lsx), tx = run(x0 + 1, y0, lsx), ty = run(x0, y0 + 1, lsx), tc = run(x0 + 2, y0 + 3, lsx);
    auto same = [&](const Trace &a) {
        if (a.nodes.size() != t0.nodes.size() || a.loads.size() != t0.loads.size() || a.stores.size() != t0.stores.size()) return false;
        for (size_t i = 0; i < a.nodes.size(); i++) if (a.nodes[i].expr != t0.nodes[i].expr) return false;
        for (size_t i = 0; i < a.stores.size(); i++) if (a.stores[i].node != t0.stores[i].node || a.stores[i].cval != t0.stores[i].cval) return false;
        return true;
    };
    if (!same(tx) || !same(ty) || !same(tc)) die("trace structure depends on invocation position");

    auto affine = [&](int64_t a, int64_t bxv, int64_t byv, int64_t cv, std::string &out) {
        int64_t cx = bxv - a, cy = byv - a, c0 = a - cx * x0 - cy * y0;
        if (c0 + cx * (x0 + 2) + cy * (y0 + 3) != cv) die("index not affine in x, y");
        char b[160];
        snprintf(b, sizeof b, "int(%lluu + %lluu * GX + %lluu * GY)", (unsigned long long)(uint32_t)c0,
                 (unsigned long long)(uint32_t)cx, (unsigned long long)(uint32_t)cy);
        out = b;
    };

    // Emission. form "std": dxil-spirv CLI interface (imageLoad/imageStore on binding 11), header taken
    // from hdr_path before ByteAddressMask/main. form "vk": vkd3d-proton interface, tensor access through
    // "<array>[<register>]._m0[i]", header taken from hdr_path before main.
    // The vk trace source uses vk-form uniform spelling, and the std source uses _18._m0 spelling; each
    // form must be emitted from a trace of a source with the same uniform spelling.
    auto emit = [&](const char *path, const char *hdr_path, bool vk, const std::string &varr, const std::string &vreg) {
        std::ifstream hf(hdr_path); std::stringstream ss; ss << hf.rdbuf(); std::string orig = ss.str();
        size_t cut = orig.find("void main()");
        if (!vk && orig.find("uint ByteAddressMask") != std::string::npos) cut = orig.find("uint ByteAddressMask");
        std::ofstream o(path);
        o << orig.substr(0, cut) << "void main()\n{\n    uint GX = gl_GlobalInvocationID.x;\n    uint GY = gl_GlobalInvocationID.y;\n";
        o << "    if (GX > " << bx << "u || GY > " << by << "u) return;\n";
        for (const std::string &u : g_ubo_used) {
            if (u[0] == 'x') o << "    if (GX >= " << u.substr(1) << ") return;\n";
            else if (u[0] == 'y') o << "    if (GY >= " << u.substr(1) << ") return;\n";
            else die("uniform used outside x/y size guard");
        }
        auto tensor = [&](const std::string &e) {
            return vk ? varr + "[" + vreg + "]._m0[" + e + "]" : std::string();
        };
        std::vector<std::string> loadexpr(t0.nodes.size());
        for (size_t i = 0; i < t0.loads.size(); i++) {
            std::string e; affine(t0.loads[i].idx, tx.loads[i].idx, ty.loads[i].idx, tc.loads[i].idx, e);
            loadexpr[t0.loads[i].node] = vk ? tensor("uint(" + e + ")") : "imageLoad(" + tname + ", " + e + ").x";
        }
        for (size_t i = 0; i < t0.nodes.size(); i++) {
            const Node &n = t0.nodes[i];
            std::string e = n.expr[0] == '@' ? loadexpr[i] : n.expr;
            o << "    " << ty_name[n.ty] << " n" << i << " = " << e << ";\n";
        }
        for (size_t i = 0; i < t0.stores.size(); i++) {
            std::string e; affine(t0.stores[i].idx, tx.stores[i].idx, ty.stores[i].idx, tc.stores[i].idx, e);
            const StoreRec &s = t0.stores[i];
            std::string v = s.node >= 0 ? "n" + std::to_string(s.node) : std::to_string(s.cval) + "u";
            if (vk) o << "    " << tensor("uint(" + e + ")") << " = " << v << ";\n";
            else o << "    imageStore(" << tname << ", " << e << ", uvec4(" << v << "));\n";
        }
        o << "}\n";
    };
    // argv[5] = output, argv[6] = header source (default: the trace source), argv[7] = "vk" or "std".
    std::string varr, vreg;
    {
        std::ifstream m(argv[4]); std::string line; std::getline(m, line);
        if (std::getline(m, line) && line.rfind("vk ", 0) == 0) {
            size_t sp = line.find(' ', 3); varr = line.substr(3, sp - 3); vreg = line.substr(sp + 1);
        }
    }
    bool vk = argc > 7 ? !strcmp(argv[7], "vk") : !varr.empty();
    emit(argv[5], argc > 6 ? argv[6] : argv[3], vk, varr, vreg);
    fprintf(stderr, "nodes %zu, loads %zu, stores %zu, MACs %ld, pruned %ld (%.1f%%)\n", t0.nodes.size(), t0.loads.size(),
            t0.stores.size(), t0.macs, t0.pruned, t0.macs ? 100.0 * t0.pruned / t0.macs : 0.0);
    return 0;
}

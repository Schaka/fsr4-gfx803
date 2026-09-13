#include <stdint.h>
#include <stdio.h>
#include <windows.h>

typedef uint64_t ffxReturnCode_t;
typedef void* ffxContext;

typedef struct ffxApiHeader {
    uint64_t type;
    struct ffxApiHeader *pNext;
} ffxApiHeader;

typedef struct {
    ffxApiHeader header;
    uint64_t createDescType;
    void *device;
    uint64_t *outputCount;
    uint64_t *versionIds;
    const char **versionNames;
} ffxQueryDescGetVersions;

typedef struct {
    ffxApiHeader header;
    uint64_t versionId;
} ffxOverrideVersion;

#define FFX_API_QUERY_DESC_TYPE_GET_VERSIONS 4u
#define FFX_API_DESC_TYPE_OVERRIDE_VERSION 5u
#define FG_VERSION_3_1_6 0xf600000000c01006ull
#define FG_CREATE_CONTEXT_TYPE 0x20001ull
#define FG_SWAPCHAIN_CREATE_TYPE 0x30006ull
#define FG_DISPATCH_PREPARE_V2 0x2000Cull
#define FG_DISPATCH_FRAMEGENERATION 0x20003ull

static ffxOverrideVersion g_override = { { FFX_API_DESC_TYPE_OVERRIDE_VERSION, NULL }, FG_VERSION_3_1_6 };

typedef ffxReturnCode_t (*PfnCreateContext)(ffxContext*, void*, const void*);
typedef ffxReturnCode_t (*PfnDestroyContext)(ffxContext*, const void*);
typedef ffxReturnCode_t (*PfnConfigure)(ffxContext*, const void*);
typedef ffxReturnCode_t (*PfnQuery)(ffxContext*, void*);
typedef ffxReturnCode_t (*PfnDispatch)(ffxContext*, const void*);

static HMODULE g_real = NULL;
static PfnCreateContext  real_CreateContext;
static PfnDestroyContext real_DestroyContext;
static PfnConfigure      real_Configure;
static PfnQuery          real_Query;
static PfnDispatch       real_Dispatch;

static void logmsg(const char *fmt_line) {
    FILE *f = fopen("Z:/data/tmp/fg_proxy.log", "a");
    if (f) { fprintf(f, "%s\n", fmt_line); fclose(f); }
}

static void ensure_loaded(void) {
    char buf[256];
    if (g_real) return;
    g_real = LoadLibraryA("amd_fidelityfx_framegeneration_dx12.dll.real");
    snprintf(buf, sizeof(buf), "LoadLibrary real dll -> %p", (void*)g_real);
    logmsg(buf);
    if (!g_real) return;
    real_CreateContext  = (PfnCreateContext)  GetProcAddress(g_real, "ffxCreateContext");
    real_DestroyContext = (PfnDestroyContext) GetProcAddress(g_real, "ffxDestroyContext");
    real_Configure       = (PfnConfigure)      GetProcAddress(g_real, "ffxConfigure");
    real_Query           = (PfnQuery)          GetProcAddress(g_real, "ffxQuery");
    real_Dispatch        = (PfnDispatch)       GetProcAddress(g_real, "ffxDispatch");
    snprintf(buf, sizeof(buf), "resolved: create=%p destroy=%p configure=%p query=%p dispatch=%p",
             (void*)real_CreateContext, (void*)real_DestroyContext, (void*)real_Configure,
             (void*)real_Query, (void*)real_Dispatch);
    logmsg(buf);
}

__declspec(dllexport) ffxReturnCode_t ffxCreateContext(ffxContext *context, void *desc, const void *memCb) {
    char buf[256];
    ensure_loaded();
    uint64_t type = desc ? *(uint64_t*)desc : 0xFFFFFFFFFFFFFFFFull;
    snprintf(buf, sizeof(buf), "ffxCreateContext type=0x%llx", (unsigned long long)type);
    logmsg(buf);
    if (type == FG_CREATE_CONTEXT_TYPE && desc) {
        ffxApiHeader *h = (ffxApiHeader*)desc;
        while (h->pNext) h = h->pNext;
        h->pNext = (ffxApiHeader*)&g_override;
        logmsg("  injected version override -> 3.1.6 (non-ML)");
    }
    if (!real_CreateContext) return 1;
    ffxReturnCode_t rc = real_CreateContext(context, desc, memCb);
    snprintf(buf, sizeof(buf), "  -> rc=%llu ctx=%p", (unsigned long long)rc, context ? *context : 0);
    logmsg(buf);
    return rc;
}

__declspec(dllexport) ffxReturnCode_t ffxDestroyContext(ffxContext *context, const void *memCb) {
    ensure_loaded();
    logmsg("ffxDestroyContext");
    if (!real_DestroyContext) return 1;
    return real_DestroyContext(context, memCb);
}

__declspec(dllexport) ffxReturnCode_t ffxConfigure(ffxContext *context, const void *desc) {
    char buf[256];
    ensure_loaded();
    uint64_t type = desc ? *(uint64_t*)desc : 0xFFFFFFFFFFFFFFFFull;
    snprintf(buf, sizeof(buf), "ffxConfigure type=0x%llx", (unsigned long long)type);
    logmsg(buf);
    if (!real_Configure) return 1;
    ffxReturnCode_t rc = real_Configure(context, desc);
    snprintf(buf, sizeof(buf), "  -> rc=%llu", (unsigned long long)rc);
    logmsg(buf);
    return rc;
}

__declspec(dllexport) ffxReturnCode_t ffxQuery(ffxContext *context, void *desc) {
    char buf[256];
    ensure_loaded();
    uint64_t type = desc ? *(uint64_t*)desc : 0xFFFFFFFFFFFFFFFFull;
    snprintf(buf, sizeof(buf), "ffxQuery type=0x%llx ctx=%p", (unsigned long long)type, context ? *context : 0);
    logmsg(buf);
    if (!real_Query) return 1;
    ffxReturnCode_t rc = real_Query(context, desc);
    snprintf(buf, sizeof(buf), "  -> rc=%llu", (unsigned long long)rc);
    logmsg(buf);

    if (type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS && rc == 0) {
        ffxQueryDescGetVersions *vq = (ffxQueryDescGetVersions*)desc;
        uint64_t n = vq->outputCount ? *vq->outputCount : 0;
        snprintf(buf, sizeof(buf), "  GET_VERSIONS count=%llu", (unsigned long long)n);
        logmsg(buf);
        for (uint64_t i = 0; i < n; i++) {
            uint64_t id = vq->versionIds ? vq->versionIds[i] : 0;
            const char *name = vq->versionNames ? vq->versionNames[i] : "(null)";
            snprintf(buf, sizeof(buf), "    [%llu] id=0x%llx name=%s", (unsigned long long)i, (unsigned long long)id, name ? name : "(null)");
            logmsg(buf);
        }
    }
    return rc;
}

__declspec(dllexport) ffxReturnCode_t ffxDispatch(ffxContext *context, const void *desc) {
    char buf[256];
    ensure_loaded();
    uint64_t type = desc ? *(uint64_t*)desc : 0xFFFFFFFFFFFFFFFFull;
    snprintf(buf, sizeof(buf), "ffxDispatch type=0x%llx", (unsigned long long)type);
    logmsg(buf);
    if (type == FG_DISPATCH_PREPARE_V2) {
        logmsg("  faking success for FG Prepare dispatch (real one fails param validation, we don't need real FG)");
        return 0; /* FFX_API_RETURN_OK, skip the real call */
    }
    if (type == FG_DISPATCH_FRAMEGENERATION) {
        logmsg("  DEBUG: faking success for real FG dispatch too (testing bogus-dispatch-size hypothesis)");
        return 0;
    }
    if (!real_Dispatch) return 1;
    return real_Dispatch(context, desc);
}

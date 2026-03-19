#include "stdafx.h"

#include "ClientTrace.h"

#include "addresses.h"
#include "ClientEntitySystem.h"

#include <stdint.h>
#include <string.h>

namespace ClientTrace {

typedef unsigned __int64 (__fastcall * cs2_client_TraceShape_t)(
    long long * param_1,
    long long param_2,
    void * param_3,
    float * param_4,
    long long * param_5,
    long long param_6
);

typedef void * (__fastcall * cs2_client_InitTraceFilter_t)(
    void * outFilter,
    const void * ignoreEnt,
    uint64_t mask,
    uint8_t mode,
    uint8_t groupByte
);

typedef void (__fastcall * cs2_client_BuildTraceHullShape_t)(
    void * dstShape,
    const void * minsMaxs
);

typedef void (__fastcall * cs2_client_TraceCollideableShape_t)(
    void * param_1,
    void * param_2,
    void * param_3,
    float * param_4,
    long long * param_5,
    void * param_6,
    void * param_7
);

static Vec3 Lerp(const Vec3 & a, const Vec3 & b, float t) {
    return {
        a.X + (b.X - a.X) * t,
        a.Y + (b.Y - a.Y) * t,
        a.Z + (b.Z - a.Z) * t
    };
}

static CEntityInstance * ResolveEntityFromHandle(SOURCESDK::CS2::CBaseHandle handle) {
    if (!handle.IsValid()) return nullptr;
    if (nullptr == g_pEntityList || nullptr == g_GetEntityFromIndex) return nullptr;

    CEntityInstance * ent = (CEntityInstance *)g_GetEntityFromIndex(*g_pEntityList, handle.GetEntryIndex());
    if (!ent) return nullptr;
    if (ent->GetHandle().ToInt() != handle.ToInt()) return nullptr;
    return ent;
}

#pragma pack(push, 1)
// stack/object blobs used by native trace paths.
// Sizes/offsets come from native callsites and result readers:
// - TraceRayBlob:   line type byte read/write at +0x28.
// - TraceFilterBlob: arg5 filter struct initialized before TraceShape call.
// - TraceResultBlob: fields read after call (normal +0x90, fraction +0xAC, startsolid +0xB7).
struct TraceRayBlob {
    uint8_t Data[0x29];
};

struct TraceFilterBlob {
    uint8_t Data[0x60];
};

struct TraceResultBlob {
    uint8_t Data[0xC0];
};

struct TraceHullBuildInput {
    Vec3 Mins;
    Vec3 Maxs;
};
#pragma pack(pop)

bool IsAvailable() {
    return 0 != AFXADDR_GET(cs2_client_TraceShape)
        && 0 != AFXADDR_GET(cs2_client_TraceContextPtr)
        && 0 != AFXADDR_GET(cs2_client_InitTraceFilter);
}

bool TraceLine(
    const Vec3 & start,
    const Vec3 & end,
    TraceResult & outResult,
    uint32_t mask,
    SOURCESDK::CS2::CBaseHandle ignoreHandle
) {
    outResult = {};
    if (!IsAvailable()) return false;

    auto fn = (cs2_client_TraceShape_t)AFXADDR_GET(cs2_client_TraceShape);
    auto ppTraceContext = (long long **)AFXADDR_GET(cs2_client_TraceContextPtr);
    auto initFilter = (cs2_client_InitTraceFilter_t)AFXADDR_GET(cs2_client_InitTraceFilter);

    if (!fn || !ppTraceContext || !*ppTraceContext || !initFilter) return false;

    long long * traceContext = *ppTraceContext;

    TraceRayBlob ray = {};
    TraceFilterBlob filter = {};
    TraceResultBlob traceResult = {};

    // Native TraceShape line path uses shape type byte at +0x28 set to 0.
    ray.Data[0x28] = 0;

    CEntityInstance * ignoreEnt = ResolveEntityFromHandle(ignoreHandle);
    // Native filter initializer/helper:
    // (outFilter, ignoreEnt, mask, mode, groupByte). For line/hull callsites:
    // mode=4 and groupByte=0x0f.
    initFilter(&filter, ignoreEnt, (uint64_t)mask, 4, 0x0f);

    Vec3 startPacked = start;
    Vec3 endPacked = end;

    fn(
        traceContext,
        (long long)&ray,
        &startPacked,
        (float *)&endPacked,
        (long long *)&filter,
        (long long)&traceResult
    );

    // Result offsets from native trace result struct reads.
    const float fraction = *(float *)&traceResult.Data[0xAC];
    const bool startSolid = 0 != traceResult.Data[0xB7];
    const bool hit = fraction < 1.0f || startSolid;
    const Vec3 normal = {
        *(float *)&traceResult.Data[0x90],
        *(float *)&traceResult.Data[0x94],
        *(float *)&traceResult.Data[0x98]
    };

    outResult.Called = true;
    outResult.Fraction = fraction;
    outResult.StartSolid = startSolid;
    outResult.Hit = hit;
    outResult.Pos = Lerp(start, end, fraction);
    outResult.HasNormal = true;
    outResult.Normal = normal;

    return true;
}

bool TraceHull(
    const Vec3 & start,
    const Vec3 & end,
    const Vec3 & mins,
    const Vec3 & maxs,
    TraceResult & outResult,
    uint32_t mask,
    SOURCESDK::CS2::CBaseHandle ignoreHandle
) {
    outResult = {};
    if (!IsAvailable()) return false;
    if (!AFXADDR_GET(cs2_client_BuildTraceHullShape)) return false;

    auto fn = (cs2_client_TraceShape_t)AFXADDR_GET(cs2_client_TraceShape);
    auto buildHull = (cs2_client_BuildTraceHullShape_t)AFXADDR_GET(cs2_client_BuildTraceHullShape);
    auto ppTraceContext = (long long **)AFXADDR_GET(cs2_client_TraceContextPtr);
    auto initFilter = (cs2_client_InitTraceFilter_t)AFXADDR_GET(cs2_client_InitTraceFilter);

    if (!fn || !buildHull || !ppTraceContext || !*ppTraceContext || !initFilter) return false;

    long long * traceContext = *ppTraceContext;

    uint8_t hullShape[0x30] = {};
    TraceHullBuildInput hullInput = { mins, maxs };
    buildHull(hullShape, &hullInput);

    TraceFilterBlob filter = {};
    TraceResultBlob traceResult = {};

    CEntityInstance * ignoreEnt = ResolveEntityFromHandle(ignoreHandle);
    initFilter(&filter, ignoreEnt, (uint64_t)mask, 4, 0x0f);

    Vec3 startPacked = start;
    Vec3 endPacked = end;

    fn(
        traceContext,
        (long long)hullShape,
        &startPacked,
        (float *)&endPacked,
        (long long *)&filter,
        (long long)&traceResult
    );

    const float fraction = *(float *)&traceResult.Data[0xAC];
    const bool startSolid = 0 != traceResult.Data[0xB7];
    const bool hit = fraction < 1.0f || startSolid;
    const Vec3 normal = {
        *(float *)&traceResult.Data[0x90],
        *(float *)&traceResult.Data[0x94],
        *(float *)&traceResult.Data[0x98]
    };

    outResult.Called = true;
    outResult.Fraction = fraction;
    outResult.StartSolid = startSolid;
    outResult.Hit = hit;
    outResult.Pos = Lerp(start, end, fraction);
    outResult.HasNormal = true;
    outResult.Normal = normal;

    return true;
}

bool TraceCollideable(
    const Vec3 & start,
    const Vec3 & end,
    SOURCESDK::CS2::CBaseHandle targetHandle,
    const Vec3 & mins,
    const Vec3 & maxs,
    TraceResult & outResult
) {
    outResult = {};

    if (!AFXADDR_GET(cs2_client_TraceCollideableShape)) return false;
    if (!AFXADDR_GET(cs2_client_TraceContextPtr)) return false;
    if (!AFXADDR_GET(cs2_client_TraceCollideableFilterPtr)) return false;
    if (!AFXADDR_GET(cs2_client_BuildTraceHullShape)) return false;

    CEntityInstance * targetEnt = ResolveEntityFromHandle(targetHandle);
    if (!targetEnt) return false;

    auto traceCollideable = (cs2_client_TraceCollideableShape_t)AFXADDR_GET(cs2_client_TraceCollideableShape);
    auto buildHull = (cs2_client_BuildTraceHullShape_t)AFXADDR_GET(cs2_client_BuildTraceHullShape);
    auto ppTraceContext = (long long **)AFXADDR_GET(cs2_client_TraceContextPtr);
    void * collideableFilterPtr = (void *)AFXADDR_GET(cs2_client_TraceCollideableFilterPtr);

    if (!traceCollideable || !buildHull || !ppTraceContext || !*ppTraceContext || !collideableFilterPtr) return false;

    void * traceContext = (void *)(*ppTraceContext);

    uint8_t hullShape[0x30] = {};
    TraceHullBuildInput hullInput = { mins, maxs };
    buildHull(hullShape, &hullInput);

    TraceResultBlob traceResult = {};
    Vec3 startPacked = start;
    Vec3 endPacked = end;

    traceCollideable(
        traceContext,
        hullShape,
        &startPacked,
        (float *)&endPacked,
        (long long *)targetEnt,
        collideableFilterPtr,
        &traceResult
    );

    const float fraction = *(float *)&traceResult.Data[0xAC];
    const bool startSolid = 0 != traceResult.Data[0xB7];
    const bool hit = fraction < 1.0f || startSolid;
    const Vec3 normal = {
        *(float *)&traceResult.Data[0x90],
        *(float *)&traceResult.Data[0x94],
        *(float *)&traceResult.Data[0x98]
    };

    outResult.Called = true;
    outResult.Fraction = fraction;
    outResult.StartSolid = startSolid;
    outResult.Hit = hit;
    outResult.Pos = Lerp(start, end, fraction);
    outResult.HasNormal = true;
    outResult.Normal = normal;

    return true;
}

}

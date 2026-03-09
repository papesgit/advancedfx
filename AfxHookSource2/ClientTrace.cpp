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

static uint32_t GetIgnoreIndexFromEntity(const CEntityInstance * ent) {
    if (!ent) return 0xffffffffU;

    const uint8_t * pEnt = (const uint8_t *)ent;
    const uint8_t * pIdentity = *(const uint8_t * const *)(pEnt + 0x10);
    if (!pIdentity) return 0xffffffffU;

    const uint32_t packedHandle = *(const uint32_t *)(pIdentity + 0x10);
    uint32_t lower = 0x7fffU;
    if (0xffffffffU != packedHandle) lower = packedHandle & 0x7fffU;

    const uint32_t serial = packedHandle >> 15;
    const uint32_t lowFlag = (*(const uint32_t *)(pIdentity + 0x30)) & 1U;
    return ((serial - lowFlag) * 0x8000U) | lower;
}

static uint16_t GetIgnoreGroupFromEntity(const CEntityInstance * ent) {
    if (!ent) return 0;

    auto pVtable = *(void ***)ent;
    if (!pVtable) return 0;

    auto getCollisionObj = (long long(__fastcall *)(const void *))pVtable[0x208 / sizeof(void *)];
    if (!getCollisionObj) return 0;

    long long collisionObj = getCollisionObj(ent);
    if (!collisionObj) return 0;
    return *(const uint16_t *)(collisionObj + 0x38);
}

#pragma pack(push, 1)
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
        && 0 != AFXADDR_GET(cs2_client_TraceFilterVft);
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
    const uint64_t filterVft = (uint64_t)AFXADDR_GET(cs2_client_TraceFilterVft);

    if (!fn || !ppTraceContext || !*ppTraceContext || !filterVft) return false;

    long long * traceContext = *ppTraceContext;

    TraceRayBlob ray = {};
    TraceFilterBlob filter = {};
    TraceResultBlob traceResult = {};

    // Native TraceShape line path uses shape type byte at +0x28 set to 0.
    ray.Data[0x28] = 0;

    CEntityInstance * ignoreEnt = ResolveEntityFromHandle(ignoreHandle);
    const uint32_t ignoreIndex = GetIgnoreIndexFromEntity(ignoreEnt);
    const uint16_t ignoreGroup = GetIgnoreGroupFromEntity(ignoreEnt);

    // Recreate native TraceShape filter layout used by line/hull callers.
    *(uint64_t *)&filter.Data[0x00] = filterVft;           // filter vft / callback table
    *(uint64_t *)&filter.Data[0x08] = (uint64_t)mask;      // trace mask
    *(uint64_t *)&filter.Data[0x10] = 0;                   // reserved
    *(uint64_t *)&filter.Data[0x18] = 0;                   // reserved
    *(uint32_t *)&filter.Data[0x20] = ignoreIndex;         // ignore entity index/id
    *(uint32_t *)&filter.Data[0x24] = 0xffffffffU;         // default include/all
    *(uint32_t *)&filter.Data[0x28] = 0xffffffffU;         // default include/all
    *(uint32_t *)&filter.Data[0x2C] = 0xffffffffU;         // default include/all
    *(uint16_t *)&filter.Data[0x30] = ignoreGroup;         // ignore collision group
    *(uint32_t *)&filter.Data[0x32] = 0xffff0000U;         // default flags
    *(uint16_t *)&filter.Data[0x36] = 0x0f00U;             // default flags
    filter.Data[0x38] = 4;                                 // query mode
    filter.Data[0x39] = 0x49;                              // query flags
    filter.Data[0x3A] = 0;                                 // single-hit mode disabled

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
    const uint64_t filterVft = (uint64_t)AFXADDR_GET(cs2_client_TraceFilterVft);

    if (!fn || !buildHull || !ppTraceContext || !*ppTraceContext || !filterVft) return false;

    long long * traceContext = *ppTraceContext;

    uint8_t hullShape[0x30] = {};
    TraceHullBuildInput hullInput = { mins, maxs };
    buildHull(hullShape, &hullInput);

    TraceFilterBlob filter = {};
    TraceResultBlob traceResult = {};

    CEntityInstance * ignoreEnt = ResolveEntityFromHandle(ignoreHandle);
    const uint32_t ignoreIndex = GetIgnoreIndexFromEntity(ignoreEnt);
    const uint16_t ignoreGroup = GetIgnoreGroupFromEntity(ignoreEnt);

    *(uint64_t *)&filter.Data[0x00] = filterVft;
    *(uint64_t *)&filter.Data[0x08] = (uint64_t)mask;
    *(uint64_t *)&filter.Data[0x10] = 0;
    *(uint64_t *)&filter.Data[0x18] = 0;
    *(uint32_t *)&filter.Data[0x20] = ignoreIndex;
    *(uint32_t *)&filter.Data[0x24] = 0xffffffffU;
    *(uint32_t *)&filter.Data[0x28] = 0xffffffffU;
    *(uint32_t *)&filter.Data[0x2C] = 0xffffffffU;
    *(uint16_t *)&filter.Data[0x30] = ignoreGroup;
    *(uint32_t *)&filter.Data[0x32] = 0xffff0000U;
    *(uint16_t *)&filter.Data[0x36] = 0x0f00U;
    filter.Data[0x38] = 4;
    filter.Data[0x39] = 0x49;
    filter.Data[0x3A] = 0;

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

#pragma once

#include <cstdint>
#include "../deps/release/prop/cs2/sdk_src/public/entityhandle.h"

namespace ClientTrace {

struct Vec3 {
    float X;
    float Y;
    float Z;
};

struct TraceResult {
    bool Called;
    float Fraction;
    bool StartSolid;
    bool Hit;
    Vec3 Pos;
    bool HasNormal;
    Vec3 Normal;
};

bool IsAvailable();

bool TraceLine(
    const Vec3 & start,
    const Vec3 & end,
    TraceResult & outResult,
    uint32_t mask = 0x80001u,
    SOURCESDK::CS2::CBaseHandle ignoreHandle = SOURCESDK::CS2::CBaseHandle()
);

bool TraceHull(
    const Vec3 & start,
    const Vec3 & end,
    const Vec3 & mins,
    const Vec3 & maxs,
    TraceResult & outResult,
    uint32_t mask = 0x80001u,
    SOURCESDK::CS2::CBaseHandle ignoreHandle = SOURCESDK::CS2::CBaseHandle()
);

bool TraceCollideable(
    const Vec3 & start,
    const Vec3 & end,
    SOURCESDK::CS2::CBaseHandle targetHandle,
    const Vec3 & mins,
    const Vec3 & maxs,
    TraceResult & outResult
);

}

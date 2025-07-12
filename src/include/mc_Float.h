#pragma once

#include "mc_Types.h"

#include <cmath>
#include <limits>

namespace mc::detail {

u32 InitFPUState();

void SetFPUState(u32 state);

// does msvc really not have 16 bit float support...
// whatever
#ifdef _MSC_VER
struct f16x4 {
    f16 values[4];

    f16x4() = default;
    f16x4(f16 x, f16 y, f16 z, f16 w) {
        values[0] = x; values[1] = y; values[2] = z; values[3] = w;
    }
};
struct f32x4 {
    f32 values[4];

    f32x4() = default;
    f32x4(float x, float y, float z, float w) {
        values[0] = x; values[1] = y; values[2] = z; values[3] = w;
    }

    f32x4 operator=(const f32x4& other) {
        std::memcpy(&this->values, &other.values, sizeof(f32x4));        
        return *this;
    }

    f32x4 operator+(const f32x4& other) const {
        return {
            this->values[0] + other.values[0],
            this->values[1] + other.values[1],
            this->values[2] + other.values[2],
            this->values[3] + other.values[3],
        };
    }

    f32x4 operator-(const f32x4& other) const {
        return {
            this->values[0] - other.values[0],
            this->values[1] - other.values[1],
            this->values[2] - other.values[2],
            this->values[3] - other.values[3],
        };
    }

    f32x4 operator*(const f32x4& other) const {
        return {
            this->values[0] * other.values[0],
            this->values[1] * other.values[1],
            this->values[2] * other.values[2],
            this->values[3] * other.values[3],
        };
    }
};
#else
typedef f16 f16x4 __attribute__ ((vector_size (8)));
typedef f32 f32x4 __attribute__ ((vector_size (16)));
#endif

union Vec4h {
    f16x4 v;
    u16 raw[4];
    f16 f[4];
};
static_assert(sizeof(Vec4h) == 0x8);

union Vec4f {
    f32x4 v;
    u32 raw[4];
    f32 f[4];
};
static_assert(sizeof(Vec4f) == 0x10);

struct Vec3 {
    f32 x, y, z;

    static constexpr f32 cEpsilon = 8 * std::numeric_limits<f32>::epsilon();

    Vec3 Cross(const Vec3& other) const {
        return {
            this->y * other.z - this->z * other.y,
            this->z * other.x - this->x * other.z,
            this->x * other.y - this->y * other.x
        };
    }

    f32 Dot(const Vec3& other) const {
        return this->x * other.x + this->y * other.y + this->z * other.z;
    }

    f32 SquaredLength() const {
        return x * x + y * y + z * z;
    }

    f32 Length() const {
        const f32 squareDist = SquaredLength();
        if (squareDist > cEpsilon * cEpsilon) {
            return std::sqrt(squareDist);
        } else {
            return 0.f;
        }
    }

    Vec3 operator+(const Vec3& other) const {
        return {
            this->x + other.x,
            this->y + other.y,
            this->z + other.z,
        };
    }
    
    Vec3 operator-(const Vec3& other) const {
        return {
            this->x - other.x,
            this->y - other.y,
            this->z - other.z,
        };
    }

    Vec3 operator*(const Vec3& other) const {
        return {
            this->x * other.x,
            this->y * other.y,
            this->z * other.z,
        };
    }

    Vec3 operator*(f32 scale) const {
        return {
            this->x * scale,
            this->y * scale,
            this->z * scale,
        };
    }

    void Normalize() {
        const f32 len = Length();
        if (len > 0.f) {
            x /= len;
            y /= len;
            z /= len;
        }
    }
};

struct Vec2 {
    float x, y;
};

} // namespace mc::detail
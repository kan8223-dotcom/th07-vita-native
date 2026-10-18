#pragma once
#include <cstddef>
#include <type_traits>

namespace touhou::modernvita {
// CPU fog processing still needs fog; the three proven fragment programs
// consume only position, diffuse and UV. Never use this with Combine/TextureMad.
struct ProvenVertex {
    float position[4];
    float diffuse[4];
    float uv[2];
    float fog[4];
};
static_assert(std::is_standard_layout<ProvenVertex>::value);
static_assert(std::is_trivially_copyable<ProvenVertex>::value);
static_assert(sizeof(ProvenVertex)==56);
static_assert(offsetof(ProvenVertex,diffuse)==16);
static_assert(offsetof(ProvenVertex,uv)==32);
static_assert(offsetof(ProvenVertex,fog)==40);
}

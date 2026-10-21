#pragma once
#include <cstddef>
#include <cstdint>

inline uint64_t unity_shader_identity(const void* control, const void* code, size_t code_size) {
    const auto* bytes = static_cast<const uint8_t*>(control);
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < 0x880; ++i)
        hash = (hash ^ (i >= 0x7d0 && i < 0x7d8 ? 0 : bytes[i])) * 1099511628211ULL;
    bytes = static_cast<const uint8_t*>(code);
    for (size_t i = 0; i < code_size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ULL;
    return hash;
}

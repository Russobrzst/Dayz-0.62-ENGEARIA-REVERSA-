#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace dz62 {
// No stack writes and no register changes on the ordinary path. The original
// leaf then executes with exactly the caller's original stack and arguments.
inline std::array<unsigned char,41> grid_dispatch(std::uintptr_t original, std::uintptr_t debug) noexcept {
    std::array<unsigned char,41> code{
        0x41,0x81,0xf8,0xe2,0x89,0xf6,0xff, // cmp r8d,-620062
        0x0f,0x84,0x0e,0,0,0,              // je debug (skip 14-byte ordinary jump)
        0xff,0x25,0,0,0,0,0,0,0,0,0,0,0,0,
        0xff,0x25,0,0,0,0,0,0,0,0,0,0,0,0
    };
    std::memcpy(code.data()+19,&original,sizeof(original));
    std::memcpy(code.data()+33,&debug,sizeof(debug));
    return code;
}
}

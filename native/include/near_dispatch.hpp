#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
namespace dz62 {
// Nine-byte patch within one 12-byte virtual thunk. The adjacent thunk is untouched.
inline bool near_dispatch(std::uintptr_t entry,std::uintptr_t pointer_slot,std::array<unsigned char,9>& patch) {
    const auto displacement=static_cast<std::int64_t>(pointer_slot)-static_cast<std::int64_t>(entry+6);
    if(displacement<(std::numeric_limits<std::int32_t>::min)() || displacement>(std::numeric_limits<std::int32_t>::max)()) return false;
    patch={0xff,0x25,0,0,0,0,0x90,0x90,0x90};
    const auto relative=static_cast<std::int32_t>(displacement);std::memcpy(patch.data()+2,&relative,4);return true;
}
}

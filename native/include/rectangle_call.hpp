#pragma once
#include "rotation_core.hpp"
namespace dz62 {
// The original engine always dereferences this argument. An insertion has no
// existing record to ignore, so pass a zero-area record rather than nullptr.
inline std::uint8_t validate_rectangle(ValidateFn fn,int width,int height,Occupancy* occupancy,
    int item_width,int item_height,int row,int col,const Record* ignored) {
    const Record empty{};
    return fn(width,height,occupancy,item_width,item_height,row,col,ignored?ignored:&empty);
}
}

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rotation_core.hpp"
#include "build_profiles.hpp"
#include "grid_dispatch.hpp"
#include "native_trace.hpp"
#include <array>
#include <cstring>
dz62::Status DZ62_PrepareBuild() noexcept;
const dz62::Profile* DZ62_SelectedProfile() noexcept;
dz62::Status DZ62_AdmitCargo(dz62::Cargo*) noexcept;
extern "C" std::uint32_t DZ62_Initialize() noexcept;
extern "C" std::uint32_t DZ62_SetExistingFootprint(dz62::Cargo*, const dz62::Request*) noexcept;
dz62::Status DZ62_InstallMoveBindings() noexcept;
int DZ62_MoveCommand(dz62::Cargo*,unsigned) noexcept;
int DZ62_RememberFootprint(void*,int,int) noexcept;
using GridFn = int (*)(dz62::Cargo*, int, int);
static GridFn original_grid = nullptr;
static int debug_grid(dz62::Cargo* cargo, int payload, int marker) noexcept {
    if (marker != -620062) return original_grid(cargo, payload, marker);
    const auto initialized = static_cast<dz62::Status>(DZ62_Initialize());
    if (initialized != dz62::Status::Applied && initialized != dz62::Status::Unchanged)
        return -static_cast<int>(initialized) - 100;
    if (payload == 0) return 620063;
    if (static_cast<unsigned>(payload)==0x40000000u) return 620068;
    if ((static_cast<unsigned>(payload)>>28)==2 || (static_cast<unsigned>(payload)>>28)==3 ||
        (static_cast<unsigned>(payload)>>28)==5 || (static_cast<unsigned>(payload)>>28)==6)
        return DZ62_MoveCommand(cargo,static_cast<unsigned>(payload));
    const auto admitted = DZ62_AdmitCargo(cargo);
    if (admitted != dz62::Status::Applied) return static_cast<int>(admitted);
    const auto bits = static_cast<unsigned int>(payload);
    if ((bits >> 28) != 1) return static_cast<int>(dz62::Status::InvalidArgument);
    const auto index = static_cast<int>(bits & 0xfff);
    if (index >= cargo->count) return static_cast<int>(dz62::Status::ItemMissing);
    const auto& r = cargo->records[index];
    const dz62::Request request{r.item,r.x,r.y,r.width,r.height,
        static_cast<std::uint8_t>((bits >> 12) & 0xff),
        static_cast<std::uint8_t>((bits >> 20) & 0xff)};
    const int cached=DZ62_RememberFootprint(r.item,request.desired_width,request.desired_height);
    if(cached!=0) return cached;
    const int result=static_cast<int>(DZ62_SetExistingFootprint(cargo,&request));
    if(result!=0 && result!=1) DZ62_RememberFootprint(r.item,request.expected_width,request.expected_height);
    return result;
}
// Invoke ONLY during suspended startup through the supplied loader. Keep DLL resident.
extern "C" __declspec(dllexport) DWORD WINAPI DZ62_InstallDebugBinding(void*) noexcept {
    const auto admitted = DZ62_PrepareBuild();
    if (admitted != dz62::Status::Applied) return static_cast<DWORD>(admitted);
    if (original_grid) return static_cast<DWORD>(dz62::Status::Unchanged);
    const auto move_status=DZ62_InstallMoveBindings();
    if(move_status!=dz62::Status::Applied && move_status!=dz62::Status::Unchanged) return static_cast<DWORD>(move_status);
    const auto* profile = DZ62_SelectedProfile();
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* entry = reinterpret_cast<unsigned char*>(base + profile->grid.rva);
    // Full function: 48 bytes, no calls or external/RIP-relative operands; all branches internal.
    if (std::memcmp(entry,profile->grid.body,sizeof(profile->grid.body)))
        return static_cast<DWORD>(dz62::Status::UnsupportedBuild);
    constexpr SIZE_T allocation_size = 48 + 41;
    auto* gateway = static_cast<unsigned char*>(VirtualAlloc(nullptr,allocation_size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if (!gateway) return static_cast<DWORD>(dz62::Status::OutOfMemory);
    std::memcpy(gateway,profile->grid.body,48);
    const auto dispatch = dz62::grid_dispatch(reinterpret_cast<std::uintptr_t>(gateway),reinterpret_cast<std::uintptr_t>(&debug_grid));
    std::memcpy(gateway+48,dispatch.data(),dispatch.size());
    DWORD protection = 0;
    if (!VirtualProtect(gateway,allocation_size,PAGE_EXECUTE_READ,&protection) || !FlushInstructionCache(GetCurrentProcess(),gateway,allocation_size)) {
        VirtualFree(gateway,0,MEM_RELEASE); return static_cast<DWORD>(dz62::Status::InvalidMemory);
    }
    std::array<unsigned char,14> jump{0xff,0x25,0,0,0,0};
    const auto destination = reinterpret_cast<std::uintptr_t>(gateway+48);
    std::memcpy(jump.data()+6,&destination,sizeof(destination));
    DWORD old = 0;
    if (!VirtualProtect(entry,jump.size(),PAGE_EXECUTE_READWRITE,&old)) {
        VirtualFree(gateway,0,MEM_RELEASE); return static_cast<DWORD>(dz62::Status::InvalidMemory);
    }
    original_grid = reinterpret_cast<GridFn>(gateway);
    std::memcpy(entry,jump.data(),jump.size());
    const bool flushed = FlushInstructionCache(GetCurrentProcess(),entry,jump.size()) != 0;
    const bool restored = VirtualProtect(entry,jump.size(),old,&protection) != 0;
    DZ62_Trace("HOOK","Grid2Index RVA=0x%x address=%p gateway=%p flushed=%d protection_restored=%d",profile->grid.rva,entry,gateway,flushed,restored);
    // On failure, loader terminates its still-suspended owned child. No live unpatch attempt.
    if (!flushed || !restored) return static_cast<DWORD>(dz62::Status::InvalidMemory);
    return static_cast<DWORD>(dz62::Status::Applied);
}

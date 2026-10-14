#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include "rotation_core.hpp"
#include "build_profiles.hpp"
#include <cstring>
#include <new>
#include <vector>

namespace {
dz62::Engine engine{};
using DirtyFn = void (*)(void*);
DirtyFn dirty = nullptr;
DWORD bound_thread = 0;
bool server_role = false;
std::uintptr_t admitted_cargo_vtable = 0;
const dz62::Profile* selected_profile = nullptr;
bool readable(const void* p, std::size_t size, bool write = false) {
    auto start = reinterpret_cast<std::uintptr_t>(p);
    if (!start || size > UINTPTR_MAX - start) return false;
    const auto end = start + size;
    while (start < end) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(reinterpret_cast<const void*>(start), &m, sizeof(m)) ||
            m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD protection = m.Protect & 0xff;
        const bool can_write = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const bool can_read = can_write || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
        if (!can_read || (write && !can_write)) return false;
        const auto next = reinterpret_cast<std::uintptr_t>(m.BaseAddress) + m.RegionSize;
        if (next <= start) return false;
        start = next;
    }
    return true;
}
bool hash_executable(char output[65]) {
    wchar_t path[32768];
    const DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    if (!n || n >= 32768) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    unsigned char digest[32], buffer[65536];
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        ok = true;
        for (;;) {
            DWORD count = 0;
            if (!ReadFile(file, buffer, sizeof(buffer), &count, nullptr)) { ok = false; break; }
            if (!count) break;
            if (BCryptHashData(hash, buffer, count, 0) < 0) { ok = false; break; }
        }
        if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (ok) {
        constexpr char hex[] = "0123456789abcdef";
        for (int i = 0; i < 32; ++i) { output[i*2] = hex[digest[i] >> 4]; output[i*2+1] = hex[digest[i] & 15]; }
        output[64] = 0;
    }
    return ok;
}
dz62::Status admit(dz62::Cargo* c, bool empty_allowed = false) {
    if (!bound_thread) return dz62::Status::NotInitialized;
    if (GetCurrentThreadId() != bound_thread) return dz62::Status::WrongThread;
    if (!readable(c, sizeof(*c), true)) return dz62::Status::InvalidMemory;
    std::uintptr_t cargo_vtable = 0;
    std::memcpy(&cargo_vtable, c, sizeof(cargo_vtable));
    if (cargo_vtable != admitted_cargo_vtable) return dz62::Status::InvalidLayout;
    if (c->count < (empty_allowed ? 0 : 1) || c->count > 4096 || c->width <= 0 || c->height <= 0 ||
        c->width > 255 || c->height > 255 || c->occupancy.length != c->width * c->height)
        return dz62::Status::InvalidLayout;
    if ((c->count && !readable(c->records, static_cast<std::size_t>(c->count) * sizeof(dz62::Record), true)) ||
        !readable(c->occupancy.cells, static_cast<std::size_t>(c->occupancy.length), true))
        return dz62::Status::InvalidMemory;
    return dz62::Status::Applied;
}
}

// Startup admission does not execute any game function or bind the loader thread.
dz62::Status DZ62_PrepareBuild() noexcept {
    if (selected_profile) return dz62::Status::Applied;
    char sha[65];
    if (!hash_executable(sha)) return dz62::Status::UnsupportedBuild;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (const auto& p : dz62::profiles) {
        if (std::strcmp(sha, p.sha256) != 0) continue;
        for (const auto& entry : p.entry) {
            const auto address = reinterpret_cast<const void*>(base + entry.rva);
            if (!readable(address, sizeof(entry.prefix)) || std::memcmp(address, entry.prefix, sizeof(entry.prefix)))
                return dz62::Status::UnsupportedBuild;
        }
        engine = {reinterpret_cast<dz62::ValidateFn>(base + p.entry[0].rva),
            reinterpret_cast<dz62::RebuildFn>(base + p.entry[1].rva),
            reinterpret_cast<dz62::FindFn>(base + p.entry[2].rva)};
        dirty = reinterpret_cast<DirtyFn>(base + p.entry[3].rva);
        server_role = p.server;
        admitted_cargo_vtable = base + (p.server ? 0xaeb898u : 0xb26ad0u);
        selected_profile = &p;
        return dz62::Status::Applied;
    }
    return dz62::Status::UnsupportedBuild;
}
const dz62::Profile* DZ62_SelectedProfile() noexcept { return selected_profile; }
dz62::Status DZ62_AdmitCargo(dz62::Cargo* c) noexcept { return admit(c); }
dz62::Status DZ62_AdmitMoveCargo(dz62::Cargo* c) noexcept { return admit(c,true); }
bool DZ62_Readable(const void* p, std::size_t n) noexcept { return readable(p,n); }
// Thread binding occurs only when called from the script/native debug entry.
extern "C" __declspec(dllexport) std::uint32_t DZ62_Initialize() noexcept {
    if (bound_thread) return static_cast<std::uint32_t>(GetCurrentThreadId() == bound_thread ?
        dz62::Status::Unchanged : dz62::Status::WrongThread);
    const auto result = DZ62_PrepareBuild();
    if (result == dz62::Status::Applied) bound_thread = GetCurrentThreadId();
    return static_cast<std::uint32_t>(result);
}
extern "C" __declspec(dllexport) std::uint32_t DZ62_SetExistingFootprint(dz62::Cargo* c, const dz62::Request* q) noexcept {
    const auto status = admit(c);
    if (status != dz62::Status::Applied) return static_cast<std::uint32_t>(status);
    if (!readable(q, sizeof(*q))) return static_cast<std::uint32_t>(dz62::Status::InvalidMemory);
    try { return static_cast<std::uint32_t>(dz62::set_existing_footprint(*c, *q, engine)); }
    catch (const std::bad_alloc&) { return static_cast<std::uint32_t>(dz62::Status::OutOfMemory); }
    catch (...) { return static_cast<std::uint32_t>(dz62::Status::EngineDisagreement); }
}
extern "C" __declspec(dllexport) std::uint32_t DZ62_ReconcileExistingFootprint(dz62::Cargo* c,const dz62::Request* q) noexcept {
    const auto status=admit(c);
    if(status!=dz62::Status::Applied) return static_cast<std::uint32_t>(status);
    if(server_role) return static_cast<std::uint32_t>(dz62::Status::RoleMismatch);
    if(!readable(q,sizeof(*q))) return static_cast<std::uint32_t>(dz62::Status::InvalidMemory);
    try {return static_cast<std::uint32_t>(dz62::reconcile_existing_footprint(*c,*q,engine));}
    catch(const std::bad_alloc&) {return static_cast<std::uint32_t>(dz62::Status::OutOfMemory);}
    catch(...) {return static_cast<std::uint32_t>(dz62::Status::EngineDisagreement);}
}
// Host must commit its script orientation BEFORE calling this explicit server-only operation.
extern "C" __declspec(dllexport) std::uint32_t DZ62_MarkScriptDirty(dz62::Cargo* c, void* item) noexcept {
    const auto status = admit(c);
    if (status != dz62::Status::Applied) return static_cast<std::uint32_t>(status);
    if (!server_role) return static_cast<std::uint32_t>(dz62::Status::RoleMismatch);
    if (!readable(item, 0x90, true) || engine.find(c, item) < 0)
        return static_cast<std::uint32_t>(dz62::Status::ItemMissing);
    const auto vtable = *reinterpret_cast<std::uintptr_t* const*>(item);
    if (!readable(vtable, 0x640))
        return static_cast<std::uint32_t>(dz62::Status::InvalidMemory);
    MEMORY_BASIC_INFORMATION memory{};
    const auto destination = reinterpret_cast<void*>(vtable[0x638/8]);
    if (!VirtualQuery(destination, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        memory.AllocationBase != GetModuleHandleW(nullptr) || (memory.Protect & PAGE_GUARD) ||
        !(memory.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return static_cast<std::uint32_t>(dz62::Status::InvalidMemory);
    dirty(item);
    return static_cast<std::uint32_t>(dz62::Status::Applied);
}

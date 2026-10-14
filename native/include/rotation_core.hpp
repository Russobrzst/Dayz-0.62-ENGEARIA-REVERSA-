#pragma once
#include <cstddef>
#include <cstdint>

namespace dz62 {
struct Record {
    std::uint8_t x, y, width, height;
    std::uint8_t unknown[4];
    void* item;
};
struct Occupancy {
    std::uint8_t* cells;
    std::int32_t length;
    std::uint32_t unknown0;
    std::int32_t capacity;
    std::uint32_t unknown1;
};
struct Cargo {
    std::uint8_t unknown0[16];
    Record* records;
    std::int32_t count;
    std::uint8_t unknown1[12];
    void* owner;
    std::int32_t width, height;
    std::uint8_t unknown2[8];
    Occupancy occupancy;
};
static_assert(sizeof(void*) == 8);
static_assert(sizeof(Record) == 16 && offsetof(Record, item) == 8);
static_assert(offsetof(Cargo, records) == 0x10 && offsetof(Cargo, count) == 0x18);
static_assert(offsetof(Cargo, owner) == 0x28 && offsetof(Cargo, width) == 0x30);
static_assert(offsetof(Cargo, occupancy) == 0x40 && sizeof(Cargo) == 0x58);
static_assert(offsetof(Occupancy, capacity) == 0x10);

// Win64 ABI, AL boolean return. No full-RAX truth test.
using ValidateFn = std::uint8_t (*)(int, int, Occupancy*, int, int, int, int, const Record*);
using RebuildFn = void (*)(Cargo*);
using FindFn = std::int32_t (*)(Cargo*, void*);
struct Engine { ValidateFn validate; RebuildFn rebuild; FindFn find; };
enum class Status : std::uint32_t {
    Applied, Unchanged, InvalidArgument, InvalidLayout, InconsistentOccupancy,
    ItemMissing, StaleRecord, InvalidDimensions, Collision, EngineDisagreement,
    PostcheckFailedRolledBack, RollbackFailed, NotInitialized, WrongThread,
    UnsupportedBuild, OutOfMemory, InvalidMemory, RoleMismatch
};
struct Request {
    void* item;
    std::uint8_t expected_x, expected_y, expected_width, expected_height;
    std::uint8_t desired_width, desired_height;
};
// Host must guarantee game-thread execution, valid spans and stable item lifetime.
Status set_existing_footprint(Cargo& cargo, const Request& request, const Engine& engine);
// Read-only preflight. The caller must use native location/move functions to commit.
Status validate_move(const Cargo& source, const Cargo& destination, const Request& request,
                     int x, int y);
bool coherent_cargo(const Cargo& cargo);
Status validate_insert(const Cargo& destination, void* item, int x, int y, int width, int height);
// Client repair of an authoritative footprint at an already matching native location.
Status reconcile_existing_footprint(Cargo& cargo, const Request& request, const Engine& engine);
}

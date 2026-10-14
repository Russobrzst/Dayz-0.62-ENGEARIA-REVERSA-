#include "rotation_core.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

namespace dz62 {
static bool layout(const Cargo& c) {
    return c.width > 0 && c.height > 0 && c.width <= 255 && c.height <= 255 &&
        c.count > 0 && c.count <= 4096 && c.records && c.occupancy.cells &&
        c.occupancy.length == c.width * c.height &&
        c.occupancy.capacity >= c.occupancy.length;
}
static bool reconstruct(const Cargo& c, std::vector<std::uint8_t>& out) {
    std::fill(out.begin(), out.end(), std::uint8_t{0});
    for (int i = 0; i < c.count; ++i) {
        const auto& r = c.records[i];
        if (!r.item || !r.width || !r.height || r.x + r.width > c.width ||
            r.y + r.height > c.height) return false;
        for (int j = 0; j < i; ++j) if (r.item == c.records[j].item) return false;
        for (int y = r.y; y < r.y + r.height; ++y)
            for (int x = r.x; x < r.x + r.width; ++x) {
                auto& cell = out[static_cast<std::size_t>(y * c.width + x)];
                if (cell) return false;
                cell = 1;
            }
    }
    return true;
}
static bool matches(const Cargo& c, const std::vector<std::uint8_t>& expected) {
    return std::equal(expected.begin(), expected.end(), c.occupancy.cells);
}
bool coherent_cargo(const Cargo& c) {
    if (c.width <= 0 || c.height <= 0 || c.width > 255 || c.height > 255 ||
        c.count < 0 || c.count > 4096 || (c.count && !c.records) || !c.occupancy.cells ||
        c.occupancy.length != c.width*c.height || c.occupancy.capacity < c.occupancy.length) return false;
    std::vector<std::uint8_t> bitmap(static_cast<std::size_t>(c.occupancy.length));
    return reconstruct(c,bitmap) && matches(c,bitmap);
}
Status validate_insert(const Cargo& destination,void* item,int x,int y,int width,int height) {
    if(!item) return Status::InvalidArgument;
    if(!coherent_cargo(destination)) return Status::InconsistentOccupancy;
    if(x<0 || y<0 || width<1 || height<1 || width>255 || height>255 ||
       x>destination.width-width || y>destination.height-height) return Status::InvalidDimensions;
    for(int i=0;i<destination.count;++i) {
        const auto& r=destination.records[i];
        if(r.item==item) return Status::InconsistentOccupancy;
        if(x<r.x+r.width && x+width>r.x && y<r.y+r.height && y+height>r.y) return Status::Collision;
    }
    return Status::Applied;
}
Status validate_move(const Cargo& source, const Cargo& destination, const Request& q, int x, int y) {
    if (!q.item) return Status::InvalidArgument;
    if (!coherent_cargo(source) || !coherent_cargo(destination)) return Status::InconsistentOccupancy;
    const Record* own = nullptr;
    for (int i=0;i<source.count;++i) if (source.records[i].item==q.item) own=&source.records[i];
    if (!own) return Status::ItemMissing;
    if (own->x != q.expected_x || own->y != q.expected_y || own->width != q.expected_width ||
        own->height != q.expected_height) return Status::StaleRecord;
    if (!((q.desired_width==own->width && q.desired_height==own->height) ||
          (q.desired_width==own->height && q.desired_height==own->width))) return Status::InvalidDimensions;
    if (x<0 || y<0 || x+q.desired_width>destination.width || y+q.desired_height>destination.height)
        return Status::InvalidDimensions;
    for (int i=0;i<destination.count;++i) {
        const auto& r=destination.records[i];
        if (&source==&destination && r.item==q.item) continue;
        if (r.item==q.item) return Status::InconsistentOccupancy;
        if (x<r.x+r.width && x+q.desired_width>r.x && y<r.y+r.height && y+q.desired_height>r.y)
            return Status::Collision;
    }
    return Status::Applied;
}
Status reconcile_existing_footprint(Cargo& c,const Request& q,const Engine& e) {
    if(!layout(c) || !q.item || !e.find || !e.validate || !e.rebuild) return Status::InvalidArgument;
    const int index=e.find(&c,q.item);
    if(index<0 || index>=c.count) return Status::ItemMissing;
    auto& r=c.records[index];
    if(r.item!=q.item || r.x!=q.expected_x || r.y!=q.expected_y || r.width!=q.expected_width || r.height!=q.expected_height)
        return Status::StaleRecord;
    if(!q.desired_width || !q.desired_height ||
       !((q.desired_width==r.width && q.desired_height==r.height) || (q.desired_width==r.height && q.desired_height==r.width)))
        return Status::InvalidDimensions;
    std::vector<Record> expected(c.records,c.records+c.count);
    expected[static_cast<std::size_t>(index)].width=q.desired_width;
    expected[static_cast<std::size_t>(index)].height=q.desired_height;
    Cargo candidate=c;candidate.records=expected.data();
    std::vector<std::uint8_t> bitmap(static_cast<std::size_t>(c.occupancy.length));
    if(!reconstruct(candidate,bitmap)) return Status::Collision;
    candidate.occupancy.cells=bitmap.data();
    if(!e.validate(c.width,c.height,&candidate.occupancy,q.desired_width,q.desired_height,r.y,r.x,&candidate.records[index]))
        return Status::EngineDisagreement;
    if(coherent_cargo(c) && r.width==q.desired_width && r.height==q.desired_height) return Status::Unchanged;
    const auto old_records=c.records;const auto old_count=c.count;const auto old_cells=c.occupancy.cells;
    const auto old_length=c.occupancy.length,old_capacity=c.occupancy.capacity;
    r.width=q.desired_width;r.height=q.desired_height;e.rebuild(&c);
    if(c.records!=old_records || c.count!=old_count || c.occupancy.cells!=old_cells ||
       c.occupancy.length!=old_length || c.occupancy.capacity!=old_capacity) return Status::RollbackFailed;
    if(std::memcmp(expected.data(),c.records,expected.size()*sizeof(Record))==0 && coherent_cargo(c)) return Status::Applied;
    return Status::RollbackFailed;
}
Status set_existing_footprint(Cargo& c, const Request& q, const Engine& e) {
    if (!q.item || !e.validate || !e.rebuild || !e.find) return Status::InvalidArgument;
    if (!layout(c)) return Status::InvalidLayout;
    std::vector<std::uint8_t> bitmap(static_cast<std::size_t>(c.occupancy.length));
    if (!reconstruct(c, bitmap) || !matches(c, bitmap)) return Status::InconsistentOccupancy;
    int index = -1;
    for (int i = 0; i < c.count; ++i) if (c.records[i].item == q.item) index = i;
    if (index < 0) return Status::ItemMissing;
    if (e.find(&c, q.item) != index) return Status::EngineDisagreement;
    auto& r = c.records[index];
    if (r.x != q.expected_x || r.y != q.expected_y || r.width != q.expected_width ||
        r.height != q.expected_height) return Status::StaleRecord;
    if (!q.desired_width || !q.desired_height) return Status::InvalidDimensions;
    if (r.width == q.desired_width && r.height == q.desired_height) return Status::Unchanged;
    if (q.desired_width != r.height || q.desired_height != r.width)
        return Status::InvalidDimensions;
    if (r.x + q.desired_width > c.width || r.y + q.desired_height > c.height)
        return Status::InvalidDimensions;
    // Independent rectangle check before the engine call: do not trust malformed bitmap/data.
    for (int y = r.y; y < r.y + q.desired_height; ++y)
        for (int x = r.x; x < r.x + q.desired_width; ++x) {
            const bool old = x < r.x + r.width && y < r.y + r.height;
            if (!old && bitmap[static_cast<std::size_t>(y * c.width + x)]) return Status::Collision;
        }
    if (!e.validate(c.width, c.height, &c.occupancy, q.desired_width, q.desired_height,
                    r.y, r.x, &r)) return Status::EngineDisagreement;
    // Allocate every host-side buffer before mutation.
    std::vector<Record> before(c.records, c.records + c.count);
    const auto original_records = c.records;
    const auto original_count = c.count;
    const auto original_cells = c.occupancy.cells;
    const auto original_length = c.occupancy.length;
    const auto original_capacity = c.occupancy.capacity;
    const auto original_width = c.width, original_height = c.height;
    const Record old = r;
    r.width = q.desired_width;
    r.height = q.desired_height;
    before[static_cast<std::size_t>(index)].width = r.width;
    before[static_cast<std::size_t>(index)].height = r.height;
    e.rebuild(&c);
    if (c.records != original_records || c.count != original_count ||
        c.occupancy.cells != original_cells || c.occupancy.length != original_length ||
        c.occupancy.capacity != original_capacity || c.width != original_width ||
        c.height != original_height) return Status::RollbackFailed;
    const bool records_ok = std::memcmp(before.data(), c.records, before.size() * sizeof(Record)) == 0;
    if (records_ok && reconstruct(c, bitmap) && matches(c, bitmap)) return Status::Applied;
    // Rollback handles a bad resulting bitmap, not arbitrary engine faults or changed ownership.
    if (!records_ok) return Status::RollbackFailed;
    r.width = old.width;
    r.height = old.height;
    before[static_cast<std::size_t>(index)] = old;
    e.rebuild(&c);
    if (c.records != original_records || c.count != original_count ||
        c.occupancy.cells != original_cells || c.occupancy.length != original_length ||
        c.occupancy.capacity != original_capacity || c.width != original_width ||
        c.height != original_height) return Status::RollbackFailed;
    if (std::memcmp(before.data(), c.records, before.size() * sizeof(Record)) == 0 &&
        reconstruct(c, bitmap) && matches(c, bitmap)) return Status::PostcheckFailedRolledBack;
    return Status::RollbackFailed;
}
}

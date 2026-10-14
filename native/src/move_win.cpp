#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rotation_core.hpp"
#include "build_profiles.hpp"
#include "move_profiles.hpp"
#include "native_trace.hpp"
#include <array>
#include <cstring>
#include <new>
#include <vector>
#include <intrin.h>
#include "near_dispatch.hpp"
#include "rectangle_call.hpp"

const dz62::Profile* DZ62_SelectedProfile() noexcept;
dz62::Status DZ62_AdmitMoveCargo(dz62::Cargo*) noexcept;
bool DZ62_Readable(const void*,std::size_t) noexcept;
extern "C" std::uint32_t DZ62_SetExistingFootprint(dz62::Cargo*,const dz62::Request*) noexcept;
extern "C" std::uint32_t DZ62_ReconcileExistingFootprint(dz62::Cargo*,const dz62::Request*) noexcept;
namespace {
using VFn=std::uint8_t (*)(dz62::Cargo*,void*,int,int,const dz62::Record*);
using AFn=std::uintptr_t (*)(dz62::Cargo*,int,int,int,int,void*);
using PFn=void (*)(void*,void*);
using InitFn=void* (*)(void*);
using BuildFn=void (*)(void*,void*,void*,int,int);
using DestroyFn=void (*)(void*);
using CopyFn=void (*)(void*,void*);
using ExecuteFn=std::uint8_t (*)(void*,void*,std::uint8_t);
VFn original_validate=nullptr; AFn original_append=nullptr; PFn original_post=nullptr;
dz62::ValidateFn rectangle=nullptr;
const dz62::MoveProfile* profile=nullptr;
InitFn location_init=nullptr; BuildFn location_build=nullptr; DestroyFn location_destroy=nullptr;
ExecuteFn execute=nullptr;
CopyFn location_copy=nullptr;
struct Scope { bool active=false; DWORD thread=0; dz62::Cargo* target=nullptr; void* item=nullptr; int w=0,h=0; } scope;
struct Pending { dz62::Cargo* source=nullptr; dz62::Request q{}; ULONGLONG tick=0; } pending;
struct Cached {void* item=nullptr; int w=0,h=0; bool initialized=false; unsigned char padding[15]{}; alignas(16) std::array<unsigned char,80> location{};};
std::array<Cached,128> cache{};
DWORD cache_thread=0;
bool cached_dimensions(void*,int&,int&);
int remember(void*,int,int);
bool active();
#ifdef DZ62_EQUIPPED_ENABLED
using CanAddFn=std::uint8_t (*)(void*,void*,int,int);
CanAddFn original_can_add=nullptr;
struct EquippedPending {void* item=nullptr;void* target=nullptr;int w=0,h=0,base_w=0,base_h=0;ULONGLONG tick=0;std::array<unsigned char,0x48> before{};} equipped;
bool install_equipped_binding(std::uintptr_t,bool);
int commit_equipped(dz62::Cargo*,int,int);
#ifdef DZ62_REEQUIP_ENABLED
int reequip(void*,void*,int);
int begin_hands(void*,void*);
int finish_hands(void*,void*);
int cancel_hands(void*,void*);
using NewValidateFn=std::uint8_t (*)(dz62::Cargo*,void*,int,int);
NewValidateFn original_new_validate=nullptr;
__declspec(noinline) std::uint8_t new_validate_binding(dz62::Cargo* cargo,void* metadata,int row,int col) {
    if(!active() || scope.target!=cargo) return original_new_validate(cargo,metadata,row,col);
    const auto result=dz62::validate_rectangle(rectangle,cargo->width,cargo->height,&cargo->occupancy,scope.w,scope.h,row,col,nullptr);
    DZ62_Trace("NEW_ITEM_VALIDATE","cargo=%p item=%p row=%d col=%d w=%d h=%d accepted=%u",static_cast<void*>(cargo),scope.item,row,col,scope.w,scope.h,result);
    return result;
}
dz62::ValidateFn original_rectangle=nullptr;
__declspec(noinline) std::uint8_t rectangle_binding(int width,int height,dz62::Occupancy* occupancy,int w,int h,int row,int col,const dz62::Record* ignored) {
    const int base_w=w,base_h=h;
    if(active() && scope.target && occupancy==&scope.target->occupancy) {w=scope.w;h=scope.h;}
    const auto result=dz62::validate_rectangle(original_rectangle,width,height,occupancy,w,h,row,col,ignored);
    if(w!=base_w || h!=base_h) DZ62_Trace("RECT_VALIDATE","caller=%p class=%dx%d desired=%dx%d row=%d col=%d accepted=%u",_ReturnAddress(),base_w,base_h,w,h,row,col,result);
    return result;
}
#endif
#endif
bool active() {return scope.active && scope.thread==GetCurrentThreadId();}
__declspec(noinline) std::uint8_t validate(dz62::Cargo* c,void* metadata,int row,int col,const dz62::Record* ignored) {
    void* caller=_ReturnAddress();
    if (!active() || scope.target!=c) {
        const auto result=original_validate(c,metadata,row,col,ignored);
        DZ62_Trace("VALIDATE_NORMAL","caller=%p cargo=%p metadata=%p row=%d col=%d accepted=%u",caller,static_cast<void*>(c),metadata,row,col,result);
        return result;
    }
    const dz62::Record* own=nullptr;
    for (int i=0;i<c->count;++i) if(c->records[i].item==scope.item) own=&c->records[i];
    const auto result=dz62::validate_rectangle(rectangle,c->width,c->height,&c->occupancy,scope.w,scope.h,row,col,own);
    DZ62_Trace(result ? "VALIDATE" : "NATIVE_ERROR","cargo=%p item=%p col=%d row=%d w=%d h=%d accepted=%u",static_cast<void*>(c),scope.item,col,row,scope.w,scope.h,result);
    return result;
}
__declspec(noinline) std::uintptr_t append(dz62::Cargo* c,int x,int y,int w,int h,void* item) {
    const int base_w=w,base_h=h;
    if(active() && c==scope.target && item==scope.item) {w=scope.w;h=scope.h;}
    else cached_dimensions(item,w,h);
    DZ62_Trace("INSERT","caller=%p cargo=%p item=%p x=%d y=%d class=%dx%d stored=%dx%d scoped=%d",_ReturnAddress(),static_cast<void*>(c),item,x,y,base_w,base_h,w,h,active());
    return original_append(c,x,y,w,h,item);
}
__declspec(noinline) void post(void* from,void* to) {
    // A committed custom RPC is mirrored locally; do not emit a second client move request.
    if(active()) {DZ62_Trace("MIRROR","native duplicate client request suppressed");return;}
    DZ62_Trace("MOVE_POST","caller=%p from=%p to=%p",_ReturnAddress(),from,to);
    original_post(from,to);
}
std::array<unsigned char,14> jump(std::uintptr_t address) {
    std::array<unsigned char,14> bytes{0xff,0x25,0,0,0,0};
    std::memcpy(bytes.data()+6,&address,8);return bytes;
}
bool hook(const dz62::MoveEntry& f,void* replacement,void** original,std::uintptr_t base) {
    auto* entry=reinterpret_cast<unsigned char*>(base+f.rva);
    auto* gateway=static_cast<unsigned char*>(VirtualAlloc(nullptr,f.span+14,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!gateway) return false;
    std::memcpy(gateway,f.bytes,f.span);const auto back=jump(base+f.rva+f.span);
    std::memcpy(gateway+f.span,back.data(),back.size());DWORD old=0,unused=0;
    if(!VirtualProtect(gateway,f.span+14,PAGE_EXECUTE_READ,&old) || !FlushInstructionCache(GetCurrentProcess(),gateway,f.span+14)) return false;
    const auto patch=jump(reinterpret_cast<std::uintptr_t>(replacement));
    if(!VirtualProtect(entry,patch.size(),PAGE_EXECUTE_READWRITE,&old)) return false;
    *original=gateway;std::memcpy(entry,patch.data(),patch.size());
    const bool installed=FlushInstructionCache(GetCurrentProcess(),entry,patch.size()) && VirtualProtect(entry,patch.size(),old,&unused);
    char hex[65]{};
    for(unsigned i=0;i<f.span;++i) std::snprintf(hex+i*2,3,"%02x",f.bytes[i]);
    DZ62_Trace(installed ? "HOOK" : "NATIVE_ERROR","RVA=0x%x address=%p gateway=%p span=%u original_bytes=%s installed=%d",f.rva,entry,gateway,f.span,hex,installed);
    return installed;
}
void* entity_link(void* link) {
    if(!DZ62_Readable(link,16)) return nullptr;
    auto* p=*reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(link)+8);
    return p ? p-0x88 : nullptr;
}
void* item_location(void* item) {
    if(!DZ62_Readable(item,profile->location_offset+8)) return nullptr;
    void* loc=*reinterpret_cast<void**>(static_cast<unsigned char*>(item)+profile->location_offset);
    return DZ62_Readable(loc,0x48) ? loc : nullptr;
}
bool location_matches(void* item,dz62::Cargo* c,int x,int y) {
    auto* loc=static_cast<unsigned char*>(item_location(item));
    return loc && *reinterpret_cast<int*>(loc)==3 &&
        *reinterpret_cast<int*>(loc+0x20)==y && *reinterpret_cast<int*>(loc+0x24)==x &&
        entity_link(*reinterpret_cast<void**>(loc+8))==c->owner &&
        entity_link(*reinterpret_cast<void**>(loc+0x10))==item;
}
bool cached_dimensions(void* item,int& w,int& h) {
    if(!cache_thread || cache_thread!=GetCurrentThreadId()) return false;
    auto* current=static_cast<unsigned char*>(item_location(item));
    if(!current) return false;
    const auto link=*reinterpret_cast<void**>(current+0x10);
    for(const auto& entry:cache) if(entry.item==item &&
        *reinterpret_cast<void* const*>(entry.location.data()+0x10)==link && entity_link(link)==item) {
        w=entry.w;h=entry.h;return true;
    }
    return false;
}
int remember(void* item,int w,int h) {
    if(!cache_thread) cache_thread=GetCurrentThreadId();
    if(cache_thread!=GetCurrentThreadId()) return static_cast<int>(dz62::Status::WrongThread);
    auto* current=static_cast<unsigned char*>(item_location(item));
    if(!current || entity_link(*reinterpret_cast<void**>(current+0x10))!=item || w<1 || h<1 || w>255 || h>255)
        return static_cast<int>(dz62::Status::InvalidArgument);
    Cached* slot=nullptr;
    const auto link=*reinterpret_cast<void**>(current+0x10);
    for(auto& entry:cache) {
        if(entry.item==item && *reinterpret_cast<void**>(entry.location.data()+0x10)==link) {slot=&entry;break;}
        if(!entry.item || entity_link(*reinterpret_cast<void**>(entry.location.data()+0x10))!=entry.item) slot=&entry;
    }
    if(!slot) return static_cast<int>(dz62::Status::OutOfMemory);
    if(!slot->initialized) {location_init(slot->location.data());slot->initialized=true;}
    // Keep native reference wrappers, not just a raw address that could be reused after deletion.
    location_copy(slot->location.data(),current);
    slot->item=item;slot->w=w;slot->h=h;
    return static_cast<int>(dz62::Status::Applied);
}
bool local_move(dz62::Cargo* to,void* item,int x,int y,int w,int h) {
    auto* from=item_location(item); if(!from) return false;
    alignas(16) std::array<unsigned char,80> destination{},source{};
    location_init(source.data());location_copy(source.data(),from);
    location_init(destination.data());
    location_build(destination.data(),to->owner,item,y,x);
    DZ62_Trace("EXECUTE","native move cargo=%p owner=%p item=%p x=%d y=%d w=%d h=%d",static_cast<void*>(to),to->owner,item,x,y,w,h);
    scope={true,GetCurrentThreadId(),to,item,w,h};
    const auto result=execute(source.data(),destination.data(),1);
    DZ62_Trace(result ? "EXECUTED" : "NATIVE_ERROR","native move accepted=%u item=%p",result,item);
    scope.active=false;location_destroy(destination.data());location_destroy(source.data());
    return result!=0;
}
int commit(dz62::Cargo* to,int x,int y) {
#ifdef DZ62_EQUIPPED_ENABLED
    if(equipped.item) return commit_equipped(to,x,y);
#endif
    const auto saved=pending;pending={};
    if(!saved.source || GetTickCount64()-saved.tick>250) return static_cast<int>(dz62::Status::StaleRecord);
    auto admitted=DZ62_AdmitMoveCargo(saved.source);
    if(admitted!=dz62::Status::Applied) return static_cast<int>(admitted);
    admitted=DZ62_AdmitMoveCargo(to); if(admitted!=dz62::Status::Applied) return static_cast<int>(admitted);
    auto status=dz62::validate_move(*saved.source,*to,saved.q,x,y);
    DZ62_Trace("PREFLIGHT","source=%p target=%p item=%p x=%d y=%d w=%u h=%u status=%u",static_cast<void*>(saved.source),static_cast<void*>(to),saved.q.item,x,y,saved.q.desired_width,saved.q.desired_height,static_cast<unsigned>(status));
    if(status!=dz62::Status::Applied) return static_cast<int>(status);
    if(!location_matches(saved.q.item,saved.source,saved.q.expected_x,saved.q.expected_y))
        return static_cast<int>(dz62::Status::EngineDisagreement);
    const int cached=remember(saved.q.item,saved.q.desired_width,saved.q.desired_height);
    if(cached!=0) return cached;
    if(to==saved.source && x==saved.q.expected_x && y==saved.q.expected_y) {
        const int result=static_cast<int>(DZ62_SetExistingFootprint(to,&saved.q));
        if(result!=0 && result!=1) remember(saved.q.item,saved.q.expected_width,saved.q.expected_height);
        return result;
    }
    const bool moved=local_move(to,saved.q.item,x,y,saved.q.desired_width,saved.q.desired_height);
    if(moved && location_matches(saved.q.item,to,x,y) && dz62::coherent_cargo(*saved.source) && dz62::coherent_cargo(*to)) {
        for(int i=0;i<to->count;++i) {
            const auto& r=to->records[i];
            if(r.item==saved.q.item && r.x==x && r.y==y && r.width==saved.q.desired_width && r.height==saved.q.desired_height)
                return static_cast<int>(dz62::Status::Applied);
        }
    }
    if(location_matches(saved.q.item,saved.source,saved.q.expected_x,saved.q.expected_y) &&
       dz62::coherent_cargo(*saved.source) && dz62::coherent_cargo(*to)) {
        remember(saved.q.item,saved.q.expected_width,saved.q.expected_height);
        return static_cast<int>(dz62::Status::EngineDisagreement);
    }
    // Roll back only a fully located item; never guess an ownership pointer after a partial engine failure.
    if(location_matches(saved.q.item,to,x,y) &&
       local_move(saved.source,saved.q.item,saved.q.expected_x,saved.q.expected_y,saved.q.expected_width,saved.q.expected_height) &&
       location_matches(saved.q.item,saved.source,saved.q.expected_x,saved.q.expected_y) &&
       dz62::coherent_cargo(*saved.source) && dz62::coherent_cargo(*to))
    {
        remember(saved.q.item,saved.q.expected_width,saved.q.expected_height);
        return static_cast<int>(dz62::Status::PostcheckFailedRolledBack);
    }
    return static_cast<int>(dz62::Status::RollbackFailed);
}
#ifdef DZ62_EQUIPPED_ENABLED
bool class_dimensions(void* item,int& w,int& h) {
    if(!DZ62_Readable(item,8)) return false;
    auto* table=*static_cast<unsigned char**>(item);
    const auto slot=profile->server?0xd50u:0xd98u;
    if(!DZ62_Readable(table,slot+8)) return false;
    using MetaFn=void* (*)(void*);
    const auto fn=*reinterpret_cast<MetaFn*>(table+slot);
    if(!fn) return false;
    auto* metadata=static_cast<unsigned char*>(fn(item));
    if(!DZ62_Readable(metadata,0x6d0)) return false;
    w=*reinterpret_cast<int*>(metadata+0x6c8);h=*reinterpret_cast<int*>(metadata+0x6cc);
    return w>0 && h>0 && w<=255 && h<=255;
}
bool equal_location(void* current,const unsigned char* expected) {
    if(!DZ62_Readable(current,0x28)) return false;
    auto* now=static_cast<unsigned char*>(current);
    return *reinterpret_cast<int*>(now)==*reinterpret_cast<const int*>(expected) &&
        entity_link(*reinterpret_cast<void**>(now+8))==entity_link(*reinterpret_cast<void* const*>(expected+8)) &&
        entity_link(*reinterpret_cast<void**>(now+0x10))==entity_link(*reinterpret_cast<void* const*>(expected+0x10)) &&
        *reinterpret_cast<int*>(now+0x18)==*reinterpret_cast<const int*>(expected+0x18) &&
        *reinterpret_cast<int*>(now+0x20)==*reinterpret_cast<const int*>(expected+0x20) &&
        *reinterpret_cast<int*>(now+0x24)==*reinterpret_cast<const int*>(expected+0x24);
}
__declspec(noinline) std::uint8_t equipped_binding(void* target,void* item,int row,int payload) {
    if(row!=-620062) return original_can_add(target,item,row,payload);
    try {
        const auto bits=static_cast<unsigned>(payload),mode=bits>>28;
#ifdef DZ62_REEQUIP_ENABLED
        if(mode==8) return reequip(target,item,static_cast<int>(bits&65535))==0?1:0;
        if(mode==9) return begin_hands(target,item)==0?1:0;
        if(mode==10) return finish_hands(target,item)==0?1:0;
        if(mode==11) return cancel_hands(target,item)==0?1:0;
#endif
        if(mode!=5 && mode!=7) return 0;
        if(mode==7) {equipped={};pending={};}
        auto* loc=static_cast<unsigned char*>(item_location(item));
        DZ62_Trace("EQUIPPED_PREFLIGHT","item=%p target=%p native_location=%p location_type=%d mode=%u",item,target,static_cast<void*>(loc),loc?*reinterpret_cast<int*>(loc):-1,mode);
        int base_w=0,base_h=0,w=static_cast<int>((bits>>12)&255),h=static_cast<int>((bits>>20)&255);
        const int kind=loc?*reinterpret_cast<int*>(loc):0;
        if(!loc || (kind!=1 && kind!=2 && kind!=4) || (kind!=1 && !entity_link(*reinterpret_cast<void**>(loc+8))) ||
           entity_link(*reinterpret_cast<void**>(loc+0x10))!=item || !class_dimensions(item,base_w,base_h) ||
           !((w==base_w && h==base_h) || (w==base_h && h==base_w))) return 0;
        if(remember(item,w,h)!=0) return 0;
        if(mode==7) {
            equipped={item,target,w,h,base_w,base_h,GetTickCount64(),{}};
            std::memcpy(equipped.before.data(),loc,equipped.before.size());
        }
        DZ62_Trace("EQUIPPED_ARM","caller=%p item=%p target=%p mode=%u w=%d h=%d",_ReturnAddress(),item,target,mode,w,h);
        return 1;
    } catch(...) {equipped={};return 0;}
}
bool install_equipped_binding(std::uintptr_t base,bool server) {
    const auto rva=server?0x3b4360u:0x3e1200u;
    auto* entry=reinterpret_cast<unsigned char*>(base+rva);
    const std::array<unsigned char,12> expected=server?
      std::array<unsigned char,12>{0x48,0x8b,0x01,0xff,0xa0,0x30,0x0f,0,0,0xcc,0xcc,0xcc}:
      std::array<unsigned char,12>{0x48,0x8b,0x01,0xff,0xa0,0x78,0x0f,0,0,0xcc,0xcc,0xcc};
    if(!DZ62_Readable(entry,expected.size()) || std::memcmp(entry,expected.data(),expected.size())) return false;
    unsigned char* gateway=nullptr;
    const auto aligned=(base+rva)&~std::uintptr_t(0xffff);
    std::array<unsigned char,9> patch{};
    for(unsigned i=1;i<32768 && !gateway;++i) {
        for(int direction : {1,-1}) {
            const auto address=static_cast<std::int64_t>(aligned)+direction*static_cast<std::int64_t>(i)*65536;
            if(address<65536 || !dz62::near_dispatch(base+rva,static_cast<std::uintptr_t>(address)+16,patch)) continue;
            gateway=static_cast<unsigned char*>(VirtualAlloc(reinterpret_cast<void*>(address),4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
            if(gateway) break;
        }
    }
    if(!gateway) return false;
    std::memcpy(gateway,expected.data(),9);
    const auto replacement=reinterpret_cast<std::uintptr_t>(&equipped_binding);
    std::memcpy(gateway+16,&replacement,8);
    DWORD old=0,unused=0;
    if(!VirtualProtect(gateway,4096,PAGE_EXECUTE_READ,&old) || !FlushInstructionCache(GetCurrentProcess(),gateway,4096)) return false;
    original_can_add=reinterpret_cast<CanAddFn>(gateway);
    if(!VirtualProtect(entry,9,PAGE_EXECUTE_READWRITE,&old)) return false;
    std::memcpy(entry,patch.data(),9);
    const bool installed=FlushInstructionCache(GetCurrentProcess(),entry,9) && VirtualProtect(entry,9,old,&unused);
    DZ62_Trace(installed?"HOOK":"NATIVE_ERROR","equipped CanAddEntityInCargoEx RVA=0x%x address=%p gateway=%p patch_span=9 adjacent_wrapper_preserved=1",rva,entry,gateway);
    return installed;
}
int commit_equipped(dz62::Cargo* to,int x,int y) {
    const auto saved=equipped;equipped={};pending={};
    struct RestoreCache {void* item;int w,h;bool keep=false;~RestoreCache(){if(!keep) remember(item,w,h);}} restore{saved.item,saved.base_w,saved.base_h};
    auto* loc=static_cast<unsigned char*>(item_location(saved.item));
    if(!loc || GetTickCount64()-saved.tick>250 || std::memcmp(loc,saved.before.data(),saved.before.size()))
        return static_cast<int>(dz62::Status::StaleRecord);
    auto status=DZ62_AdmitMoveCargo(to);
    if(status!=dz62::Status::Applied) return static_cast<int>(status);
    if(to->owner!=saved.target) return static_cast<int>(dz62::Status::EngineDisagreement);
    status=dz62::validate_insert(*to,saved.item,x,y,saved.w,saved.h);
    if(status!=dz62::Status::Applied) {remember(saved.item,saved.base_w,saved.base_h);return static_cast<int>(status);}
    if(!dz62::validate_rectangle(rectangle,to->width,to->height,&to->occupancy,saved.w,saved.h,y,x,nullptr)) return static_cast<int>(dz62::Status::EngineDisagreement);
    alignas(16) std::array<unsigned char,80> before{};
    location_init(before.data());location_copy(before.data(),loc);
    const bool moved=local_move(to,saved.item,x,y,saved.w,saved.h);
    bool matched=false;
    if(moved && location_matches(saved.item,to,x,y) && dz62::coherent_cargo(*to))
        for(int i=0;i<to->count;++i) {const auto& r=to->records[i];if(r.item==saved.item && r.x==x && r.y==y && r.width==saved.w && r.height==saved.h) matched=true;}
    int result=static_cast<int>(dz62::Status::Applied);
    if(!matched) {
        remember(saved.item,saved.base_w,saved.base_h);
        auto* now=item_location(saved.item);
        if(equal_location(now,before.data())) result=static_cast<int>(dz62::Status::EngineDisagreement);
        else if(location_matches(saved.item,to,x,y)) {
            alignas(16) std::array<unsigned char,80> current{};
            location_init(current.data());location_copy(current.data(),now);
            scope={true,GetCurrentThreadId(),nullptr,saved.item,saved.base_w,saved.base_h};
            const bool restored=execute(current.data(),before.data(),1)!=0;scope.active=false;
            location_destroy(current.data());now=item_location(saved.item);
            result=static_cast<int>(restored && equal_location(now,before.data()) && dz62::coherent_cargo(*to)?dz62::Status::PostcheckFailedRolledBack:dz62::Status::RollbackFailed);
        } else result=static_cast<int>(dz62::Status::RollbackFailed);
    }
    location_destroy(before.data());
    restore.keep=result==0;
    DZ62_Trace(result==0?"EQUIPPED_COMMIT":"NATIVE_ERROR","item=%p target=%p x=%d y=%d w=%d h=%d status=%d",saved.item,static_cast<void*>(to),x,y,saved.w,saved.h,result);
    return result;
}
#ifdef DZ62_REEQUIP_ENABLED
bool attachment_matches(void* item,void* actor,int slot) {
    auto* loc=static_cast<unsigned char*>(item_location(item));
    return loc && *reinterpret_cast<int*>(loc)==(slot==-1?4:2) && (slot==-1 || *reinterpret_cast<int*>(loc+0x18)==slot) &&
        entity_link(*reinterpret_cast<void**>(loc+8))==actor && entity_link(*reinterpret_cast<void**>(loc+0x10))==item;
}
struct HandsPending {
    Pending source{};
    void* actor=nullptr;
    int base_w=0,base_h=0;
    std::vector<dz62::Record> records;
} hands_pending;
int begin_hands(void* actor,void* item) {
    const auto saved=pending;pending={};equipped={};
    if(!saved.source || saved.q.item!=item || GetTickCount64()-saved.tick>250) return static_cast<int>(dz62::Status::StaleRecord);
    const auto admitted=DZ62_AdmitMoveCargo(saved.source);
    if(admitted!=dz62::Status::Applied) return static_cast<int>(admitted);
    if(!location_matches(item,saved.source,saved.q.expected_x,saved.q.expected_y) || !dz62::coherent_cargo(*saved.source)) return static_cast<int>(dz62::Status::EngineDisagreement);
    if(hands_pending.actor && GetTickCount64()-hands_pending.source.tick<5000) return static_cast<int>(dz62::Status::StaleRecord);
    bool matched=false;
    for(int i=0;i<saved.source->count;++i) {
        const auto& r=saved.source->records[i];
        if(r.item==item && r.x==saved.q.expected_x && r.y==saved.q.expected_y && r.width==saved.q.expected_width && r.height==saved.q.expected_height) matched=true;
    }
    int base_w=0,base_h=0;
    if(!matched || !DZ62_Readable(actor,8) || !class_dimensions(item,base_w,base_h)) return static_cast<int>(dz62::Status::StaleRecord);
    const std::vector<dz62::Record> records(saved.source->records,saved.source->records+saved.source->count);
    const int cached=remember(item,base_w,base_h);if(cached!=0) return cached;
    hands_pending={saved,actor,base_w,base_h,records};
    DZ62_Trace("HANDS_BEGIN","actor=%p item=%p source=%p awaiting stock hand transition",actor,item,static_cast<void*>(saved.source));
    return 0;
}
int finish_hands(void* actor,void* item) {
    if(hands_pending.actor!=actor || hands_pending.source.q.item!=item || !hands_pending.source.source) return static_cast<int>(dz62::Status::StaleRecord);
    const auto& saved=hands_pending.source;
    if(!attachment_matches(item,actor,-1)) {
        if(GetTickCount64()-saved.tick>=5000) {
            if(location_matches(item,saved.source,saved.q.expected_x,saved.q.expected_y)) remember(item,saved.q.expected_width,saved.q.expected_height);
            DZ62_Trace("HANDS_TIMEOUT","actor=%p item=%p source=%p",actor,item,static_cast<void*>(saved.source));
            hands_pending={};
        }
        return static_cast<int>(dz62::Status::EngineDisagreement);
    }
    const auto admitted=DZ62_AdmitMoveCargo(saved.source);
    if(admitted!=dz62::Status::Applied) return static_cast<int>(admitted);
    bool removed=saved.source->count+1==static_cast<int>(hands_pending.records.size());
    for(int i=0;i<saved.source->count && removed;++i) {
        const auto& r=saved.source->records[i];bool preserved=false;
        for(const auto& before:hands_pending.records) {
            if(before.item!=item && r.item==before.item && r.x==before.x && r.y==before.y && r.width==before.width && r.height==before.height) preserved=true;
        }
        if(!preserved) removed=false;
    }
    if(!removed) return static_cast<int>(dz62::Status::StaleRecord);
    if(!dz62::coherent_cargo(*saved.source)) {
        const auto* build=DZ62_SelectedProfile();
        const auto rebuild=reinterpret_cast<dz62::RebuildFn>(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))+build->entry[1].rva);
        rebuild(saved.source);
    }
    if(!dz62::coherent_cargo(*saved.source)) return static_cast<int>(dz62::Status::InconsistentOccupancy);
    DZ62_Trace("TAKE_HANDS","actor=%p item=%p source=%p verified=1 status=0",actor,item,static_cast<void*>(saved.source));
    hands_pending={};return 0;
}
int cancel_hands(void* actor,void* item) {
    if(hands_pending.actor!=actor || hands_pending.source.q.item!=item) return static_cast<int>(dz62::Status::StaleRecord);
    const auto saved=hands_pending.source;
    if(location_matches(item,saved.source,saved.q.expected_x,saved.q.expected_y)) remember(item,saved.q.expected_width,saved.q.expected_height);
    hands_pending={};return 0;
}
int reequip(void* actor,void* item,int slot) {
    const auto saved=pending;pending={};equipped={};
    if(!saved.source || saved.q.item!=item || GetTickCount64()-saved.tick>250) return static_cast<int>(dz62::Status::StaleRecord);
    const auto admitted=DZ62_AdmitMoveCargo(saved.source);
    if(admitted!=dz62::Status::Applied) return static_cast<int>(admitted);
    if(!location_matches(item,saved.source,saved.q.expected_x,saved.q.expected_y)) return static_cast<int>(dz62::Status::EngineDisagreement);
    bool matched=false;
    for(int i=0;i<saved.source->count;++i) {const auto& r=saved.source->records[i];if(r.item==item && r.x==saved.q.expected_x && r.y==saved.q.expected_y && r.width==saved.q.expected_width && r.height==saved.q.expected_height) matched=true;}
    int base_w=0,base_h=0;
    if(!matched || !dz62::coherent_cargo(*saved.source) || !DZ62_Readable(actor,8) || !class_dimensions(item,base_w,base_h)) return static_cast<int>(dz62::Status::StaleRecord);
    const std::vector<dz62::Record> original_records(saved.source->records,saved.source->records+saved.source->count);
    alignas(16) std::array<unsigned char,80> from{},to{};
    location_init(from.data());location_copy(from.data(),item_location(item));
    location_init(to.data());location_build(to.data(),actor,item,0,0);
    // Native construction supplies the entity reference wrappers. The retained
    // InventoryLocation dispatcher uses type 2 plus slot for attachments,
    // and type 4 plus the actor/item links for hands.
    *reinterpret_cast<int*>(to.data())=slot==-1?4:2;*reinterpret_cast<int*>(to.data()+0x18)=slot;
    const int cached=remember(item,base_w,base_h);
    int result=static_cast<int>(dz62::Status::EngineDisagreement);
    if(cached==0 && entity_link(*reinterpret_cast<void**>(to.data()+8))==actor && entity_link(*reinterpret_cast<void**>(to.data()+0x10))==item) {
        scope={true,GetCurrentThreadId(),nullptr,item,base_w,base_h};
        const bool accepted=execute(from.data(),to.data(),1)!=0;scope.active=false;
        if(accepted && attachment_matches(item,actor,slot)) {
            bool removed=saved.source->count+1==static_cast<int>(original_records.size());
            for(int i=0;i<saved.source->count && removed;++i) {
                const auto& r=saved.source->records[i];bool preserved=false;
                for(const auto& before:original_records) if(before.item!=item && r.item==before.item && r.x==before.x && r.y==before.y && r.width==before.width && r.height==before.height) preserved=true;
                if(!preserved) removed=false;
            }
            if(removed && !dz62::coherent_cargo(*saved.source)) {
                const auto* build=DZ62_SelectedProfile();
                const auto native_rebuild=reinterpret_cast<dz62::RebuildFn>(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))+build->entry[1].rva);
                native_rebuild(saved.source);
                DZ62_Trace("REEQUIP_BITMAP","cargo=%p remaining=%d coherent=%d",static_cast<void*>(saved.source),saved.source->count,dz62::coherent_cargo(*saved.source));
            }
            if(removed && dz62::coherent_cargo(*saved.source)) result=0;
        }
        if(result!=0) {
            remember(item,saved.q.expected_width,saved.q.expected_height);
            if(equal_location(item_location(item),from.data())) result=static_cast<int>(dz62::Status::EngineDisagreement);
            else if(attachment_matches(item,actor,slot) && local_move(saved.source,item,saved.q.expected_x,saved.q.expected_y,saved.q.expected_width,saved.q.expected_height) &&
                    location_matches(item,saved.source,saved.q.expected_x,saved.q.expected_y) && dz62::coherent_cargo(*saved.source)) result=static_cast<int>(dz62::Status::PostcheckFailedRolledBack);
            else result=static_cast<int>(dz62::Status::RollbackFailed);
        }
    }
    location_destroy(to.data());location_destroy(from.data());
    DZ62_Trace(result==0?(slot==-1?"TAKE_HANDS":"REEQUIP"):"NATIVE_ERROR","actor=%p item=%p slot=%d source=%p status=%d",actor,item,slot,static_cast<void*>(saved.source),result);
    return result;
}
#endif
#endif
}
dz62::Status DZ62_InstallMoveBindings() noexcept {
    const auto* build=DZ62_SelectedProfile();if(!build) return dz62::Status::NotInitialized;
    if(profile) return dz62::Status::Unchanged;
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto& selected=dz62::move_profiles[build->server?1:0];
#ifdef DZ62_REEQUIP_ENABLED
    DZ62_Trace("INSTALL","movement debug v12 asynchronous hands confirmation role=%s image=%p sha256=%s",build->server?"SERVER":"CLIENT",reinterpret_cast<void*>(base),build->sha256);
#elif defined(DZ62_RECTANGLE_FIXED)
    DZ62_Trace("INSTALL","movement debug v7 equipped rectangle fix role=%s image=%p sha256=%s",build->server?"SERVER":"CLIENT",reinterpret_cast<void*>(base),build->sha256);
#elif defined(DZ62_EQUIPPED_ENABLED)
    DZ62_Trace("INSTALL","movement debug v6 equipped role=%s image=%p sha256=%s",build->server?"SERVER":"CLIENT",reinterpret_cast<void*>(base),build->sha256);
#else
    DZ62_Trace("INSTALL","movement debug v5 role=%s image=%p sha256=%s",build->server?"SERVER":"CLIENT",reinterpret_cast<void*>(base),build->sha256);
#endif
    for(const auto& f:selected.f) if(f.rva && (!DZ62_Readable(reinterpret_cast<void*>(base+f.rva),f.span) ||
       std::memcmp(reinterpret_cast<void*>(base+f.rva),f.bytes,f.span))) return dz62::Status::UnsupportedBuild;
    profile=&selected;rectangle=reinterpret_cast<dz62::ValidateFn>(base+build->entry[0].rva);
    location_init=reinterpret_cast<InitFn>(base+selected.f[3].rva);
    location_build=reinterpret_cast<BuildFn>(base+selected.f[4].rva);
    location_destroy=reinterpret_cast<DestroyFn>(base+selected.f[5].rva);
    execute=reinterpret_cast<ExecuteFn>(base+selected.f[6].rva);
    location_copy=reinterpret_cast<CopyFn>(base+selected.f[7].rva);
    if(!hook(selected.f[0],reinterpret_cast<void*>(&validate),reinterpret_cast<void**>(&original_validate),base) ||
       !hook(selected.f[1],reinterpret_cast<void*>(&append),reinterpret_cast<void**>(&original_append),base) ||
       (!build->server && !hook(selected.f[2],reinterpret_cast<void*>(&post),reinterpret_cast<void**>(&original_post),base)))
        return dz62::Status::InvalidMemory;
#ifdef DZ62_EQUIPPED_ENABLED
    if(!install_equipped_binding(base,build->server)) return dz62::Status::InvalidMemory;
#ifdef DZ62_REEQUIP_ENABLED
    dz62::MoveEntry rectangle_entry{build->entry[0].rva,15,{}};
    std::memcpy(rectangle_entry.bytes,build->entry[0].prefix,15);
    if(!hook(rectangle_entry,reinterpret_cast<void*>(&rectangle_binding),reinterpret_cast<void**>(&original_rectangle),base)) return dz62::Status::InvalidMemory;
    rectangle=original_rectangle;
    dz62::MoveEntry new_entry{build->server?0x3e9f8cu:0x41c798u,15,{0x48,0x83,0xec,0x48,0x8b,0x82,0xcc,0x06,0,0,0x44,0x89,0x4c,0x24,0x30}};
    if(!hook(new_entry,reinterpret_cast<void*>(&new_validate_binding),reinterpret_cast<void**>(&original_new_validate),base)) return dz62::Status::InvalidMemory;
#endif
#endif
    return dz62::Status::Applied;
}
int DZ62_MoveCommand(dz62::Cargo* c,unsigned bits) noexcept {
    DZ62_Trace("COMMAND","cargo=%p opcode=%u payload=0x%08x",static_cast<void*>(c),bits>>28,bits);
    try {
        if(!profile) return static_cast<int>(dz62::Status::NotInitialized);
        const auto admitted=DZ62_AdmitMoveCargo(c);if(admitted!=dz62::Status::Applied) {
#ifdef DZ62_EQUIPPED_ENABLED
            if((bits>>28)==3 && equipped.item) {remember(equipped.item,equipped.base_w,equipped.base_h);equipped={};}
#endif
            return static_cast<int>(admitted);
        }
        if((bits>>28)==5 || (bits>>28)==6) {
            const unsigned index=bits&0xfff;
            if(index>=static_cast<unsigned>(c->count)) return static_cast<int>(dz62::Status::ItemMissing);
            const auto r=c->records[index];
            const int w=static_cast<int>((bits>>12)&255),h=static_cast<int>((bits>>20)&255);
            if(!((w==r.width && h==r.height) || (w==r.height && h==r.width)) ||
               !location_matches(r.item,c,r.x,r.y)) return static_cast<int>(dz62::Status::EngineDisagreement);
            const int cached=remember(r.item,w,h);if(cached!=0) return cached;
            if((bits>>28)==5) return cached;
            const dz62::Request q{r.item,r.x,r.y,r.width,r.height,static_cast<std::uint8_t>(w),static_cast<std::uint8_t>(h)};
            const int result=static_cast<int>(DZ62_ReconcileExistingFootprint(c,&q));
            DZ62_Trace("RECONCILE","item=%p x=%u y=%u w=%d h=%d status=%d",r.item,r.x,r.y,w,h,result);
            if(result!=0 && result!=1) remember(r.item,r.width,r.height);
            return result;
        }
        if((bits>>28)==2) {
#ifdef DZ62_EQUIPPED_ENABLED
            equipped={};
#endif
            pending={};const unsigned index=bits&0xfff;
            if(index>=static_cast<unsigned>(c->count)) return static_cast<int>(dz62::Status::ItemMissing);
            const auto& r=c->records[index];
            pending={c,{r.item,r.x,r.y,r.width,r.height,static_cast<std::uint8_t>((bits>>12)&255),static_cast<std::uint8_t>((bits>>20)&255)},GetTickCount64()};
            return static_cast<int>(dz62::Status::Applied);
        }
        if((bits>>28)==3) {
            const int result=commit(c,static_cast<int>(bits&255),static_cast<int>((bits>>8)&255));
            DZ62_Trace("COMMIT","cargo=%p x=%u y=%u status=%d",static_cast<void*>(c),bits&255,(bits>>8)&255,result);
            return result;
        }
        return static_cast<int>(dz62::Status::InvalidArgument);
    } catch(const std::bad_alloc&) {pending={};scope.active=false;return static_cast<int>(dz62::Status::OutOfMemory);}
      catch(...) {pending={};scope.active=false;return static_cast<int>(dz62::Status::EngineDisagreement);}
}
int DZ62_RememberFootprint(void* item,int w,int h) noexcept {
    return remember(item,w,h);
}

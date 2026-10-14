#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "native_trace.hpp"
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {
constexpr unsigned capacity=512,line_size=2048;
std::array<std::array<char,line_size>,capacity> lines{};
unsigned head=0,tail=0,count=0;
SRWLOCK lock=SRWLOCK_INIT;
INIT_ONCE once=INIT_ONCE_STATIC_INIT;
HANDLE event_handle=nullptr;
volatile LONG dropped=0;
DWORD WINAPI writer(void*) {
    wchar_t path[32768];HANDLE file=INVALID_HANDLE_VALUE;
    const auto length=GetModuleFileNameW(nullptr,path,32768);
    if(length && length<32768) {
        auto* slash=std::wcsrchr(path,L'\\');
        if(slash && !wcscpy_s(slash+1,32768-static_cast<std::size_t>(slash+1-path),L"trial_profile\\native_rotation.log"))
            file=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    }
    for(;;) {
        WaitForSingleObject(event_handle,INFINITE);
        for(;;) {
            std::array<char,line_size> current{};
            AcquireSRWLockExclusive(&lock);
            const bool found=count!=0;
            if(found) {current=lines[tail];tail=(tail+1)%capacity;--count;}
            ReleaseSRWLockExclusive(&lock);
            if(!found) break;
            if(file!=INVALID_HANDLE_VALUE) {
                DWORD written=0;WriteFile(file,current.data(),static_cast<DWORD>(std::strlen(current.data())),&written,nullptr);
                const LONG lost=InterlockedExchange(&dropped,0);
                if(lost) {
                    char notice[128];const int n=std::snprintf(notice,sizeof(notice),"[NATIVE_ERROR] trace queue full; dropped=%ld events\r\n",lost);
                    if(n>0) WriteFile(file,notice,static_cast<DWORD>(n),&written,nullptr);
                }
            }
        }
    }
}
BOOL CALLBACK initialize(PINIT_ONCE,void*,void**) {
    event_handle=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(event_handle) {
        HANDLE thread=CreateThread(nullptr,0,&writer,nullptr,0,nullptr);
        if(thread) CloseHandle(thread);
        else {CloseHandle(event_handle);event_handle=nullptr;}
    }
    return TRUE;
}
}
void DZ62_Trace(const char* event,const char* format,...) noexcept {
    const DWORD previous_error=GetLastError();
    InitOnceExecuteOnce(&once,&initialize,nullptr,nullptr);
    if(!event_handle) {SetLastError(previous_error);return;}
    char body[1500];va_list args;va_start(args,format);
    std::vsnprintf(body,sizeof(body),format,args);va_end(args);body[sizeof(body)-1]=0;
    SYSTEMTIME stamp{};GetLocalTime(&stamp);
    std::array<char,line_size> line{};
    std::snprintf(line.data(),line.size(),"[%02u:%02u:%02u.%03u][PID=%lu TID=%lu][%s] %s\r\n",
        stamp.wHour,stamp.wMinute,stamp.wSecond,stamp.wMilliseconds,GetCurrentProcessId(),GetCurrentThreadId(),event,body);
    AcquireSRWLockExclusive(&lock);
    if(count<capacity) {lines[head]=line;head=(head+1)%capacity;++count;}
    else InterlockedIncrement(&dropped);
    ReleaseSRWLockExclusive(&lock);SetEvent(event_handle);SetLastError(previous_error);
}

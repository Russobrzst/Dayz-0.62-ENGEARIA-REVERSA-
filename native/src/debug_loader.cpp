#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include "rotation_core.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static std::wstring quote(const std::wstring& text) {
    std::wstring result = L"\""; std::size_t slashes = 0;
    for (const auto ch : text) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"') { result.append(slashes*2+1,L'\\'); result += ch; }
        else { result.append(slashes,L'\\'); result += ch; }
        slashes = 0;
    }
    result.append(slashes*2,L'\\'); result += L'\"'; return result;
}
static std::uintptr_t remote_module(DWORD pid, const std::wstring& identity, bool exact_path) {
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 10; ++i) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,pid);
        if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
        Sleep(20);
    }
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry); std::uintptr_t result = 0;
    if (Module32FirstW(snapshot,&entry)) do {
        if (_wcsicmp(exact_path ? entry.szExePath : entry.szModule,identity.c_str()) == 0) {
            result = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr); break;
        }
    } while (Module32NextW(snapshot,&entry));
    CloseHandle(snapshot); return result;
}
static bool run_remote(HANDLE process, std::uintptr_t entry, void* argument, DWORD& exit_code) {
    HANDLE thread = CreateRemoteThread(process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(entry),argument,0,nullptr);
    if (!thread) return false;
    const auto waited = WaitForSingleObject(thread,30000);
    const bool result = waited == WAIT_OBJECT_0 && GetExitCodeThread(thread,&exit_code);
    CloseHandle(thread); return result;
}
static bool write_byte(HANDLE process, std::uintptr_t address, unsigned char value) {
    DWORD old = 0, unused = 0; SIZE_T written = 0;
    auto* destination = reinterpret_cast<void*>(address);
    if (!VirtualProtectEx(process,destination,1,PAGE_EXECUTE_READWRITE,&old)) return false;
    const bool wrote = WriteProcessMemory(process,destination,&value,1,&written) && written == 1;
    const bool flushed = FlushInstructionCache(process,destination,1) != 0;
    const bool restored = VirtualProtectEx(process,destination,1,old,&unused) != 0;
    return wrote && flushed && restored;
}
static std::uint32_t entry_rva(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary); IMAGE_DOS_HEADER dos{};
    input.read(reinterpret_cast<char*>(&dos),sizeof(dos));
    if (!input || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 1024*1024) return 0;
    IMAGE_NT_HEADERS64 header{}; input.seekg(dos.e_lfanew);
    input.read(reinterpret_cast<char*>(&header),sizeof(header));
    if (!input || header.Signature != IMAGE_NT_SIGNATURE || header.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        header.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        header.OptionalHeader.AddressOfEntryPoint >= header.OptionalHeader.SizeOfImage) return 0;
    return header.OptionalHeader.AddressOfEntryPoint;
}
static bool pause_before_entry(PROCESS_INFORMATION& child, std::uint32_t entry_offset) {
    const auto deadline = GetTickCount64()+30000;
    std::vector<HANDLE> handles;
    std::uintptr_t entry = 0;
    unsigned char original = 0;
    bool armed = false;
    while (GetTickCount64() < deadline) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event,500)) {
            if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
            return false;
        }
        DWORD continuation = DBG_CONTINUE;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            entry = reinterpret_cast<std::uintptr_t>(event.u.CreateProcessInfo.lpBaseOfImage)+entry_offset;
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            handles.push_back(event.u.CreateProcessInfo.hProcess);
            handles.push_back(event.u.CreateProcessInfo.hThread);
        } else if (event.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
            handles.push_back(event.u.CreateThread.hThread);
        } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
        } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            if (armed && event.dwThreadId == child.dwThreadId &&
                event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT &&
                reinterpret_cast<std::uintptr_t>(event.u.Exception.ExceptionRecord.ExceptionAddress) == entry) {
                CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
                if (!write_byte(child.hProcess,entry,original) || !GetThreadContext(child.hThread,&context) ||
                    context.Rip != entry+1) return false;
                context.Rip = entry;
                if (!SetThreadContext(child.hThread,&context) ||
                    SuspendThread(child.hThread) == static_cast<DWORD>(-1)) return false;
                if (!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,DBG_CONTINUE) ||
                    !DebugActiveProcessStop(child.dwProcessId)) return false;
                for (const auto handle : handles)
                    if (handle && handle != child.hProcess && handle != child.hThread) CloseHandle(handle);
                return true;
            }
            MEMORY_BASIC_INFORMATION memory{};
            const auto ntdll = remote_module(child.dwProcessId,L"ntdll.dll",false);
            const bool initial = event.dwThreadId == child.dwThreadId &&
                event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && ntdll &&
                VirtualQueryEx(child.hProcess,event.u.Exception.ExceptionRecord.ExceptionAddress,&memory,sizeof(memory)) &&
                reinterpret_cast<std::uintptr_t>(memory.AllocationBase) == ntdll;
            if (initial) {
                SIZE_T read = 0;
                if (armed || !entry || !ReadProcessMemory(child.hProcess,reinterpret_cast<void*>(entry),&original,1,&read) ||
                    read != 1 || !write_byte(child.hProcess,entry,0xcc)) return false;
                armed = true;
                continuation = DBG_CONTINUE;
            } else {
                continuation = DBG_EXCEPTION_NOT_HANDLED;
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            ContinueDebugEvent(event.dwProcessId,event.dwThreadId,DBG_CONTINUE);
            return false;
        }
        if (!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,continuation)) return false;
    }
    return false;
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--self-test") {
        const auto dll = LoadLibraryW(argv[2]);
        if (!dll) { std::cerr << "FAIL DLL load\n"; return 1; }
        using Install = DWORD (WINAPI*)(void*);
        const auto install = reinterpret_cast<Install>(GetProcAddress(dll,"DZ62_InstallDebugBinding"));
        const bool rejected = install && install(nullptr) == static_cast<DWORD>(dz62::Status::UnsupportedBuild);
        const bool quoting = quote(L"a b") == L"\"a b\"" && quote(L"x\\") == L"\"x\\\\\"" &&
            quote(L"a\"b") == L"\"a\\\"b\"";
        FreeLibrary(dll);
        if (!rejected || !quoting) { std::cerr << "FAIL loader self-test\n"; return 1; }
        std::cout << "PASS loader self-test: foreign host rejected; Windows argument quoting; no game launched\n";
        return 0;
    }
    const bool startup_test = argc == 4 && std::wstring(argv[1]) == L"--startup-test";
    const int first = startup_test ? 2 : 1;
    if (argc < first+2) {
        std::cerr << "Usage: debug_loader.exe <supported-game.exe> <probe.dll> [game arguments...]\n";
        return 2;
    }
    try {
        const auto exe = std::filesystem::canonical(argv[first]);
        const auto dll = std::filesystem::canonical(argv[first+1]);
        if (startup_test && exe.filename() != L"probe_tests.exe") return 1;
        const auto entry_offset = entry_rva(exe);
        if (!entry_offset) { std::cerr << "Unsupported PE entry\n"; return 1; }
        // Export RVA inspection only: this does not execute the DLL locally.
        const auto local_image = LoadLibraryExW(dll.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);
        if (!local_image) { std::cerr << "Cannot inspect DLL exports\n"; return 1; }
        const auto install = GetProcAddress(local_image,"DZ62_InstallDebugBinding");
        const auto install_rva = reinterpret_cast<std::uintptr_t>(install) - reinterpret_cast<std::uintptr_t>(local_image);
        FreeLibrary(local_image);
        if (!install) { std::cerr << "Debug export missing\n"; return 1; }
        const auto load_library = GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");
        HMODULE implementation = nullptr;
        if (!load_library || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(load_library),&implementation)) return 1;
        wchar_t system_path[32768];
        if (!GetModuleFileNameW(implementation,system_path,32768)) return 1;
        const auto system_name = std::filesystem::path(system_path).filename().wstring();
        const auto load_rva = reinterpret_cast<std::uintptr_t>(load_library) - reinterpret_cast<std::uintptr_t>(implementation);
        std::wstring command = quote(exe.wstring());
        for (int i = first+2; i < argc; ++i) { command += L' '; command += quote(argv[i]); }
        std::vector<wchar_t> mutable_command(command.begin(),command.end()); mutable_command.push_back(0);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        if (startup_test || exe.filename().wstring().find(L"DayZServer") == 0) {
            startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        }
        PROCESS_INFORMATION child{};
        const auto directory = exe.parent_path();
        if (!CreateProcessW(exe.c_str(),mutable_command.data(),nullptr,nullptr,FALSE,DEBUG_ONLY_THIS_PROCESS,
                nullptr,directory.c_str(),&startup,&child)) {
            std::cerr << "CreateProcess failed: " << GetLastError() << '\n'; return 1;
        }
        // Every failure cleanup below addresses only THIS freshly created, suspended child.
        auto fail = [&](const char* reason) {
            std::cerr << reason << "; owned child PID " << child.dwProcessId << '\n';
            if (!TerminateProcess(child.hProcess,1)) std::cerr << "Owned-child termination failed: " << GetLastError() << '\n';
            DebugActiveProcessStop(child.dwProcessId);
            WaitForSingleObject(child.hProcess,5000);
            CloseHandle(child.hThread); CloseHandle(child.hProcess); return 1;
        };
        if (!pause_before_entry(child,entry_offset)) return fail("Executable entry breakpoint unavailable");
        const auto remote_system = remote_module(child.dwProcessId,system_name,false);
        if (!remote_system) return fail("System module unavailable before startup");
        const auto path = dll.wstring(); const auto bytes = (path.size()+1)*sizeof(wchar_t);
        void* remote_path = VirtualAllocEx(child.hProcess,nullptr,bytes,MEM_COMMIT | MEM_RESERVE,PAGE_READWRITE);
        if (!remote_path) return fail("Remote path allocation failed");
        SIZE_T written = 0;
        if (!WriteProcessMemory(child.hProcess,remote_path,path.c_str(),bytes,&written) || written != bytes)
            return fail("Remote path write failed");
        DWORD loaded = 0;
        if (!run_remote(child.hProcess,remote_system+load_rva,remote_path,loaded))
            return fail("DLL load thread failed or timed out");
        VirtualFreeEx(child.hProcess,remote_path,0,MEM_RELEASE);
        // Thread exit is DWORD: do not use its truncated value as a 64-bit HMODULE.
        const auto remote_dll = remote_module(child.dwProcessId,path,true);
        if (!remote_dll) return fail("DLL was not loaded into owned child");
        DWORD installed = 0;
        if (!run_remote(child.hProcess,remote_dll+install_rva,nullptr,installed))
            return fail("Installer failed or timed out");
        if (startup_test && installed == static_cast<DWORD>(dz62::Status::UnsupportedBuild)) {
            const bool stopped = TerminateProcess(child.hProcess,0) &&
                WaitForSingleObject(child.hProcess,5000) == WAIT_OBJECT_0;
            CloseHandle(child.hThread); CloseHandle(child.hProcess);
            if (!stopped) return 1;
            std::cout << "PASS startup test: own fixture paused, DLL loaded, foreign build rejected, child stopped before entry\n";
            return 0;
        }
        if (installed != static_cast<DWORD>(dz62::Status::Applied) && installed != static_cast<DWORD>(dz62::Status::Unchanged)) {
            std::cerr << "Installer status " << installed << '\n'; return fail("Build or entry rejected");
        }
        if (startup_test) return fail("Unexpected fixture admission");
        if (ResumeThread(child.hThread) == static_cast<DWORD>(-1)) return fail("Resume failed");
        std::cout << "STARTED PID=" << child.dwProcessId << " debug binding installed before main thread resumed\n";
        CloseHandle(child.hThread); CloseHandle(child.hProcess); return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

#pragma once
// Bounded asynchronous logging; the game thread never writes the file.
#ifdef DZ62_TRACE_ENABLED
void DZ62_Trace(const char* event,const char* format,...) noexcept;
#else
inline void DZ62_Trace(const char*,const char*,...) noexcept {}
#endif

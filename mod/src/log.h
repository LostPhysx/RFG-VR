#pragma once

namespace rfgvr::log {

void init(const wchar_t* path);
void write(const char* fmt, ...);

}  // namespace rfgvr::log

#define LOG(...) ::rfgvr::log::write(__VA_ARGS__)

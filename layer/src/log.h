#pragma once
#include <cstdarg>
#include <cstdio>

namespace bdex {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void log_set_level(int level);
int  log_get_level();
void log_set_file(const char* path);
void log_write(LogLevel lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

} // namespace bdex

#define BDEX_LOG(lvl, ...)  ::bdex::log_write(::bdex::LogLevel::lvl, __VA_ARGS__)
#define BDEX_ERR(...)       BDEX_LOG(Error, __VA_ARGS__)
#define BDEX_WARN(...)      BDEX_LOG(Warn, __VA_ARGS__)
#define BDEX_INFO(...)      BDEX_LOG(Info, __VA_ARGS__)
#define BDEX_DBG(...)       BDEX_LOG(Debug, __VA_ARGS__)

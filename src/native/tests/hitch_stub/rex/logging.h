// Host-test stub for src/hitch_catcher.cpp (hitch-catcher-test.cpp): REXLOG_* print to stdout.
#pragma once
#include <cstdio>
#include <format>
#define REXLOG_INFO(...) (std::puts(std::format(__VA_ARGS__).c_str()), std::fflush(stdout))
#define REXLOG_WARN(...) (std::puts(std::format(__VA_ARGS__).c_str()), std::fflush(stdout))

#pragma once

namespace sof::log {

	void Open(const char* path);

	void Write(const char* format, ...);

	void SetThreadName(const char* name);

}

#define SOF_LOG(...) ::sof::log::Write(__VA_ARGS__)

#pragma once
#include <cstdint>

namespace symbolize {
bool Symbolize(intptr_t addr, char* buf, int buf_size);
}

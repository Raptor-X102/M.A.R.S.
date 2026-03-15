#pragma once

#if defined(__linux__)
#include "os_linux.hpp"
#else
#error "Unsupported OS"
#endif

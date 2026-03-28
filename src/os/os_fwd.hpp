#pragma once

#include <stdexcept>
#include <string>

namespace os {

class SystemError : public std::runtime_error {
public:
    explicit SystemError(const std::string& msg) : std::runtime_error(msg) {}
};

class PermissionError : public SystemError {
public:
    explicit PermissionError(const std::string& msg) : SystemError(msg) {}
};

class ResourceError : public SystemError {
public:
    explicit ResourceError(const std::string& msg) : SystemError(msg) {}
};

} // namespace os

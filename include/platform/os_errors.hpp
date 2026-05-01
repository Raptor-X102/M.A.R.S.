#pragma once

#include <stdexcept>
#include <string>

namespace silicon_probe::platform {

class SystemError : public std::runtime_error {
   public:
    explicit SystemError(const std::string& message) : std::runtime_error(message) {}
};

class PermissionError : public SystemError {
   public:
    explicit PermissionError(const std::string& message) : SystemError(message) {}
};

class ResourceError : public SystemError {
   public:
    explicit ResourceError(const std::string& message) : SystemError(message) {}
};

}  // namespace silicon_probe::platform

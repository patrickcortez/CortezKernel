/**
 * @file result.hpp
 * @brief Freestanding Status codes and Result<T> error-handling monad (zero exceptions).
 */

#pragma once

#include "types.hpp"

namespace cortez {

/**
 * @brief POSIX-aligned kernel status and error codes.
 */
enum class Status : i32 {
    Ok                  = 0,
    InvalidArgument     = -22,  // -EINVAL
    OutOfMemory         = -12,  // -ENOMEM
    IoError             = -5,   // -EIO
    NotFound            = -2,   // -ENOENT
    AlreadyExists       = -17,  // -EEXIST
    NotDirectory        = -20,  // -ENOTDIR
    IsDirectory         = -21,  // -EISDIR
    NoSpace             = -28,  // -ENOSPC
    NotSupported        = -95,  // -EOPNOTSUPP
    Timeout             = -110, // -ETIMEDOUT
    DeviceNotFound      = -19,  // -ENODEV
    CorruptedFs         = -117, // -EUCLEAN
    BadFileDescriptor   = -9,   // -EBADF
    DirectoryNotEmpty   = -39,  // -ENOTEMPTY
    Busy                = -16   // -EBUSY
};

/**
 * @brief Converts a Status code to a human-readable string.
 */
constexpr const char* statusToString(Status status) noexcept {
    switch (status) {
        case Status::Ok:                return "Success";
        case Status::InvalidArgument:   return "Invalid argument (EINVAL)";
        case Status::OutOfMemory:       return "Out of memory (ENOMEM)";
        case Status::IoError:           return "Input/output error (EIO)";
        case Status::NotFound:          return "No such file or directory (ENOENT)";
        case Status::AlreadyExists:     return "File exists (EEXIST)";
        case Status::NotDirectory:      return "Not a directory (ENOTDIR)";
        case Status::IsDirectory:       return "Is a directory (EISDIR)";
        case Status::NoSpace:           return "No space left on device (ENOSPC)";
        case Status::NotSupported:      return "Operation not supported (EOPNOTSUPP)";
        case Status::Timeout:           return "Operation timed out (ETIMEDOUT)";
        case Status::DeviceNotFound:    return "No such device (ENODEV)";
        case Status::CorruptedFs:       return "Filesystem structure corrupted (EUCLEAN)";
        case Status::BadFileDescriptor: return "Bad file descriptor (EBADF)";
        case Status::DirectoryNotEmpty: return "Directory not empty (ENOTEMPTY)";
        case Status::Busy:              return "Device or resource busy (EBUSY)";
        default:                        return "Unknown kernel error";
    }
}

/**
 * @brief Lightweight value-or-error wrapper for freestanding C++ (-fno-exceptions).
 */
template <typename T>
class CORTEZ_NODISCARD Result final {
public:
    constexpr Result(const T& value) noexcept
        : m_value(value), m_status(Status::Ok) {}

    constexpr Result(Status errorStatus) noexcept
        : m_value{}, m_status(errorStatus) {}

    constexpr bool isOk() const noexcept { return m_status == Status::Ok; }
    constexpr bool isError() const noexcept { return m_status != Status::Ok; }
    constexpr Status status() const noexcept { return m_status; }

    constexpr const T& value() const noexcept { return m_value; }
    constexpr T& value() noexcept { return m_value; }
    constexpr T valueOr(const T& fallback) const noexcept {
        return isOk() ? m_value : fallback;
    }

private:
    T      m_value;
    Status m_status;
};

} // namespace cortez

#include "apex/platform/file_output.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace apex::platform {
namespace {

std::filesystem::path writeTemporaryExclusive(
    const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    constexpr std::size_t maxAttempts = 1'024U;
    for (std::size_t attempt = 0; attempt < maxAttempts; ++attempt) {
        auto temporary = path;
        temporary += ".apex-tmp-" + std::to_string(attempt);

#if defined(_WIN32)
        const HANDLE handle = CreateFileW(
            temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
                continue;
            throw std::system_error(static_cast<int>(error),
                                    std::system_category(),
                                    "cannot create " + temporary.string());
        }
        std::size_t offset = 0;
        bool succeeded = true;
        while (offset < bytes.size()) {
            const auto remaining = bytes.size() - offset;
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
                remaining, static_cast<std::size_t>(
                               std::numeric_limits<DWORD>::max())));
            DWORD written = 0;
            if (WriteFile(handle, bytes.data() + offset, chunk, &written,
                          nullptr) == FALSE ||
                written == 0U) {
                succeeded = false;
                break;
            }
            offset += written;
        }
        if (succeeded) succeeded = FlushFileBuffers(handle) != FALSE;
        const auto writeError = succeeded ? ERROR_SUCCESS : GetLastError();
        const bool closed = CloseHandle(handle) != FALSE;
        if (!succeeded || !closed) {
            std::error_code ignored;
            (void)std::filesystem::remove(temporary, ignored);
            const auto error =
                writeError != ERROR_SUCCESS ? writeError : GetLastError();
            throw std::system_error(static_cast<int>(error),
                                    std::system_category(),
                                    "cannot write " + temporary.string());
        }
#else
        const int descriptor = ::open(
            temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (descriptor < 0) {
            if (errno == EEXIST) continue;
            throw std::system_error(errno, std::generic_category(),
                                    "cannot create " + temporary.string());
        }
        std::size_t offset = 0;
        int writeError = 0;
        while (offset < bytes.size()) {
            const auto remaining = bytes.size() - offset;
            const auto chunk = std::min<std::size_t>(
                remaining, static_cast<std::size_t>(
                               std::numeric_limits<ssize_t>::max()));
            const auto written =
                ::write(descriptor, bytes.data() + offset, chunk);
            if (written < 0) {
                if (errno == EINTR) continue;
                writeError = errno;
                break;
            }
            if (written == 0) {
                writeError = EIO;
                break;
            }
            offset += static_cast<std::size_t>(written);
        }
        if (writeError == 0 && ::fsync(descriptor) != 0) writeError = errno;
        if (::close(descriptor) != 0 && writeError == 0) writeError = errno;
        if (writeError != 0) {
            std::error_code ignored;
            (void)std::filesystem::remove(temporary, ignored);
            throw std::system_error(writeError, std::generic_category(),
                                    "cannot write " + temporary.string());
        }
#endif
        return temporary;
    }
    throw std::runtime_error("cannot allocate a temporary output beside " +
                             path.string());
}

void promoteTemporaryNoReplace(const std::filesystem::path& temporary,
                               const std::filesystem::path& path) {
#if defined(_WIN32)
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) ==
        FALSE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
            throw std::runtime_error("output already exists: " +
                                     path.string());
        throw std::system_error(static_cast<int>(error),
                                std::system_category(),
                                "cannot finalize " + path.string());
    }
#else
    if (::link(temporary.c_str(), path.c_str()) != 0) {
        if (errno == EEXIST)
            throw std::runtime_error("output already exists: " +
                                     path.string());
        throw std::system_error(errno, std::generic_category(),
                                "cannot finalize " + path.string());
    }
    if (::unlink(temporary.c_str()) != 0) {
        const auto cleanupError = errno;
        throw std::system_error(
            cleanupError, std::generic_category(),
            "cannot remove temporary output " + temporary.string());
    }
#endif
}

void promoteTemporaryReplace(const std::filesystem::path& temporary,
                             const std::filesystem::path& path) {
#if defined(_WIN32)
    if (MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
        FALSE) {
        const auto error = GetLastError();
        throw std::system_error(static_cast<int>(error),
                                std::system_category(),
                                "cannot replace " + path.string());
    }
#else
    if (::rename(temporary.c_str(), path.c_str()) != 0)
        throw std::system_error(errno, std::generic_category(),
                                "cannot replace " + path.string());
#endif
}

template <typename Promote>
void writeFile(const std::filesystem::path& path,
               std::span<const std::uint8_t> bytes, Promote promote) {
    if (path.empty()) throw std::runtime_error("output path is empty");
    const auto temporary = writeTemporaryExclusive(path, bytes);
    try {
        promote(temporary, path);
    } catch (...) {
        std::error_code ignored;
        (void)std::filesystem::remove(temporary, ignored);
        throw;
    }
}

}  // namespace

void writeFileExclusive(const std::filesystem::path& path,
                        std::span<const std::uint8_t> bytes) {
    writeFile(path, bytes, promoteTemporaryNoReplace);
}

void writeFileExclusive(const std::filesystem::path& path,
                        std::string_view text) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
    writeFileExclusive(path,
                       std::span<const std::uint8_t>(data, text.size()));
}

void writeFileAtomicReplace(const std::filesystem::path& path,
                            std::span<const std::uint8_t> bytes) {
    writeFile(path, bytes, promoteTemporaryReplace);
}

}  // namespace apex::platform

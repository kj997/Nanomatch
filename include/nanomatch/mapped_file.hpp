#pragma once
#include "platform.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>

#if defined(__linux__) || defined(__APPLE__)
  #include <fcntl.h>
  #include <unistd.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #define NM_HAS_POSIX_MMAP 1
#elif defined(_WIN32)
  // <windows.h> brings in CreateFileA / CreateFileMappingA / MapViewOfFile
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #define NM_HAS_POSIX_MMAP 0
#else
  #define NM_HAS_POSIX_MMAP 0
#endif

namespace nanomatch {

// ─────────────────────────────────────────────────────────────────────────
// MappedFile — zero-copy read-only view of a file.
//
// Linux/macOS: mmap with POSIX_FADV_SEQUENTIAL hint for kernel readahead.
// Windows:     CreateFileMapping + MapViewOfFile.
// Fallback:    plain read() into heap buffer.
//
// API:
//   open(path)  → ok/fail
//   data()      → const std::byte*  start of mapped region
//   size()      → std::size_t       total bytes
//   close()     → release
// ─────────────────────────────────────────────────────────────────────────

class MappedFile {
public:
    MappedFile() noexcept = default;
    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile() noexcept { close(); }

    // Open a file read-only and map it. Returns false on any failure.
    [[nodiscard]] bool open(const char* path) noexcept {
        close();
#if NM_HAS_POSIX_MMAP
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) return false;

        struct stat st{};
        if (::fstat(fd, &st) != 0) { ::close(fd); return false; }
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ == 0) { ::close(fd); return false; }

        void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        // POSIX_FADV — best-effort, ignore failure
  #if defined(POSIX_FADV_SEQUENTIAL)
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
  #endif
        ::close(fd);   // safe — mapping holds its own reference

        if (p == MAP_FAILED) { size_ = 0; return false; }
        data_ = static_cast<const std::byte*>(p);
        backend_ = "mmap(POSIX)";
        return true;

#elif defined(_WIN32)
        HANDLE hFile = CreateFileA(path, GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                   nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(hFile, &sz) || sz.QuadPart == 0) {
            CloseHandle(hFile); return false;
        }
        size_ = static_cast<std::size_t>(sz.QuadPart);

        HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!hMap) { CloseHandle(hFile); size_ = 0; return false; }

        void* p = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        win_file_ = hFile;
        win_map_  = hMap;

        if (!p) {
            CloseHandle(hMap); CloseHandle(hFile);
            win_file_ = win_map_ = nullptr; size_ = 0; return false;
        }
        data_ = static_cast<const std::byte*>(p);
        backend_ = "MapViewOfFile(Win32)";
        return true;

#else
        // Fallback: read whole file into heap. Loses zero-copy but works anywhere.
        FILE* f = std::fopen(path, "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n <= 0) { std::fclose(f); return false; }
        size_ = static_cast<std::size_t>(n);
        owned_buf_ = static_cast<std::byte*>(std::malloc(size_));
        if (!owned_buf_) { std::fclose(f); size_ = 0; return false; }
        std::fread(owned_buf_, 1, size_, f);
        std::fclose(f);
        data_ = owned_buf_;
        backend_ = "fread(fallback)";
        return true;
#endif
    }

    void close() noexcept {
        if (!data_) return;
#if NM_HAS_POSIX_MMAP
        ::munmap(const_cast<std::byte*>(data_), size_);
#elif defined(_WIN32)
        UnmapViewOfFile(data_);
        if (win_map_)  CloseHandle(win_map_);
        if (win_file_) CloseHandle(win_file_);
        win_map_ = win_file_ = nullptr;
#else
        std::free(owned_buf_);
        owned_buf_ = nullptr;
#endif
        data_ = nullptr;
        size_ = 0;
        backend_ = "closed";
    }

    [[nodiscard]] const std::byte* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t      size() const noexcept { return size_; }
    [[nodiscard]] bool             is_open() const noexcept { return data_ != nullptr; }
    [[nodiscard]] const char*      backend() const noexcept { return backend_; }

private:
    const std::byte* data_    = nullptr;
    std::size_t      size_    = 0;
    const char*      backend_ = "closed";

#if defined(_WIN32) && !NM_HAS_POSIX_MMAP
    HANDLE win_file_ = nullptr;
    HANDLE win_map_  = nullptr;
#elif !NM_HAS_POSIX_MMAP
    std::byte* owned_buf_ = nullptr;
#endif
};

} // namespace nanomatch
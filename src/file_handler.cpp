/**
 * @file file_handler.cpp
 * @brief Definitions for file handling functions.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <cerrno>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

// local includes
#include "file_handler.h"
#include "logging.h"

namespace file_handler {
#ifdef SUNSHINE_TESTS
  namespace test_support {
    static std::atomic<FailStage> current_fail_stage {FailStage::None};

    void set_failure_injection(FailStage stage) {
      current_fail_stage.store(stage, std::memory_order_relaxed);
    }

    void reset_failure_injection() {
      current_fail_stage.store(FailStage::None, std::memory_order_relaxed);
    }

    FailStage get_failure_injection() {
      return current_fail_stage.load(std::memory_order_relaxed);
    }
  }  // namespace test_support
#endif

namespace {
#ifdef _WIN32
  /**
   * @brief RAII guard to safely manage and close a Windows file handle.
   */
  struct HandleGuard {
    HANDLE handle {INVALID_HANDLE_VALUE};  ///< Managed Windows file handle.

    /**
     * @brief Construct handle guard.
     * @param h File handle to manage.
     */
    explicit HandleGuard(HANDLE h) noexcept : handle(h) {}

    /**
     * @brief Destructor closing handle if valid.
     */
    ~HandleGuard() noexcept {
      close();
    }

    /**
     * @brief Close the managed handle, retaining ownership if Windows reports a failure.
     * @return True if the handle is closed or was already invalid.
     */
    bool close() noexcept {
      if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
        if (!CloseHandle(handle)) {
          return false;
        }
        handle = INVALID_HANDLE_VALUE;
      }
      return true;
    }

    HandleGuard(const HandleGuard &) = delete;
    HandleGuard &operator=(const HandleGuard &) = delete;
  };
#else
  /**
   * @brief RAII guard to safely manage and close a POSIX file descriptor.
   */
  struct FdGuard {
    int fd {-1};  ///< Managed POSIX file descriptor.

    /**
     * @brief Construct descriptor guard.
     * @param f File descriptor to manage.
     */
    explicit FdGuard(int f) noexcept : fd(f) {}

    /**
     * @brief Destructor closing descriptor if open.
     */
    ~FdGuard() noexcept {
      close();
    }

    /**
     * @brief Close the managed descriptor without retrying an ambiguous close failure.
     * @return True if close succeeded or the descriptor was already invalid.
     */
    bool close() noexcept {
      if (fd >= 0) {
        const auto owned_fd = fd;
        fd = -1;
        return ::close(owned_fd) == 0;
      }
      return true;
    }

    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;
  };
#endif

  /**
   * @brief RAII guard to safely clean up a staged temporary file on error.
   *
   * Holds the path by reference rather than copying to guarantee no memory allocation
   * or exception can occur during guard construction.
   */
  struct TempCleanupGuard {
    const std::filesystem::path &path_ref;  ///< Reference to the staged temporary file path.
    bool armed {true};                     ///< Flag indicating if cleanup should execute.

    /**
     * @brief Construct temporary cleanup guard by reference.
     * @param p Temporary path reference.
     */
    explicit TempCleanupGuard(const std::filesystem::path &p) noexcept : path_ref(p) {}

    /**
     * @brief Disarms the guard upon successful completion of atomic replacement.
     */
    void disarm() noexcept {
      armed = false;
    }

    /**
     * @brief Destructor unlinks the staged temporary file if armed.
     */
    ~TempCleanupGuard() noexcept {
      if (armed) {
#ifdef _WIN32
        DeleteFileW(path_ref.c_str());
#else
        unlink(path_ref.c_str());
#endif
      }
    }

    TempCleanupGuard(const TempCleanupGuard &) = delete;
    TempCleanupGuard &operator=(const TempCleanupGuard &) = delete;
  };
}  // namespace

  std::string get_parent_directory(const std::string &path) {
    // remove any trailing path separators
    std::string trimmed_path = path;
    while (!trimmed_path.empty() && trimmed_path.back() == '/') {
      trimmed_path.pop_back();
    }

    std::filesystem::path p(trimmed_path);
    return p.parent_path().string();
  }

  bool make_directory(const std::string &path) {
    // first, check if the directory already exists
    if (std::filesystem::exists(path)) {
      return true;
    }

    return std::filesystem::create_directories(path);
  }

  std::string read_file(const std::filesystem::path &path) {
    if (!std::filesystem::exists(path)) {
      BOOST_LOG(debug) << "Missing file: " << path;
      return {};
    }

    std::ifstream in(path);
    return std::string {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
  }

  std::string read_file(const char *path) {
    if (!path) {
      return {};
    }
    std::error_code ec;
    std::filesystem::path p(reinterpret_cast<const char8_t *>(path));
    if (!std::filesystem::exists(p, ec)) {
      p = std::filesystem::path(path);
    }
    return read_file(p);
  }

  int write_file(const char *path, const std::string_view &contents) {
    std::ofstream out(path);

    if (!out.is_open()) {
      return -1;
    }

    out << contents;

    return 0;
  }

  int write_file_atomic(const std::filesystem::path &path, const std::string_view &contents) {
    try {
      if (path.empty() || path.filename().empty()) {
        BOOST_LOG(error) << "Invalid empty target path for atomic write";
        return -1;
      }

      const auto parent_dir = path.parent_path();
      if (!parent_dir.empty()) {
        std::error_code ec;
        if (!std::filesystem::exists(parent_dir, ec) || !std::filesystem::is_directory(parent_dir, ec)) {
          BOOST_LOG(error) << "Parent directory does not exist for atomic write: " << parent_dir;
          return -1;
        }
      }

      static std::atomic<uint64_t> counter {0};
      const auto current_count = counter.fetch_add(1, std::memory_order_relaxed);
      std::random_device rd;
      std::mt19937_64 gen(rd());
      std::uniform_int_distribution<uint64_t> dis;

      std::filesystem::path temp_path;

#ifdef _WIN32
      // Validate existing target: fail-closed on directory or reparse point (symlink/junction)
      const DWORD attrs = GetFileAttributesW(path.c_str());
      bool target_exists = false;
      if (attrs != INVALID_FILE_ATTRIBUTES) {
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
          BOOST_LOG(error) << "Target is a directory, refusing atomic write: " << path;
          return -1;
        }
        if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
          BOOST_LOG(error) << "Target is a reparse point (symlink/junction), refusing atomic write: " << path;
          return -1;
        }
        target_exists = true;
      } else {
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
          BOOST_LOG(error) << "GetFileAttributesW failed on target " << path << ": " << err;
          return -1;
        }
      }

      // Preserve existing DACL if target exists, applying it directly at CreateFileW
      // to avoid exposing credentials through broad parent-inherited ACLs.
      std::vector<BYTE> sd_buf;
      SECURITY_ATTRIBUTES sa {};
      SECURITY_ATTRIBUTES *p_sa = nullptr;

      if (target_exists) {
        const SECURITY_INFORMATION sec_info = DACL_SECURITY_INFORMATION;
        DWORD needed = 0;
        GetFileSecurityW(path.c_str(), sec_info, nullptr, 0, &needed);
        const DWORD last_err = GetLastError();
        if (needed > 0) {
          sd_buf.resize(needed);
          if (!GetFileSecurityW(path.c_str(), sec_info, reinterpret_cast<PSECURITY_DESCRIPTOR>(sd_buf.data()), needed, &needed)) {
            BOOST_LOG(error) << "GetFileSecurityW failed on target " << path << ": " << GetLastError();
            return -1;
          }
          sa.nLength = sizeof(SECURITY_ATTRIBUTES);
          sa.lpSecurityDescriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(sd_buf.data());
          sa.bInheritHandle = FALSE;
          p_sa = &sa;
        } else {
          BOOST_LOG(error) << "GetFileSecurityW query length failed on target " << path << ": " << last_err;
          return -1;
        }
      }

      HANDLE h_file = INVALID_HANDLE_VALUE;
      for (int attempt = 0; attempt < 50; ++attempt) {
        const auto rand_val = dis(gen);
        const auto temp_name = std::format(L"{}.tmp.{:x}.{:x}", path.filename().native(), current_count, rand_val);
        temp_path = parent_dir.empty() ? std::filesystem::path(temp_name) : (parent_dir / temp_name);

        h_file = CreateFileW(
          temp_path.c_str(),
          GENERIC_READ | GENERIC_WRITE,
          0,
          p_sa,
          CREATE_NEW,
          FILE_ATTRIBUTE_NORMAL,
          nullptr
        );

        if (h_file != INVALID_HANDLE_VALUE) {
          break;
        }

        const DWORD err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) {
          BOOST_LOG(error) << "CreateFileW failed creating temp file " << temp_path << ": " << err;
          return -1;
        }
      }

      if (h_file == INVALID_HANDLE_VALUE) {
        BOOST_LOG(error) << "Failed to exclusively create unique temp file after retries for " << path;
        return -1;
      }

      TempCleanupGuard temp_guard {temp_path};
      HandleGuard handle_guard {h_file};

#ifdef SUNSHINE_TESTS
      if (test_support::get_failure_injection() == test_support::FailStage::AfterPartialWrite) {
        const size_t partial_len = contents.size() > 1 ? (contents.size() / 2) : (contents.empty() ? 0 : 1);
        if (partial_len > 0) {
          DWORD written = 0;
          WriteFile(h_file, contents.data(), static_cast<DWORD>(partial_len), &written, nullptr);
        }
        handle_guard.close();
        return -1;
      }
#endif

      if (!contents.empty()) {
        DWORD written = 0;
        size_t total_written = 0;
        const char *data_ptr = contents.data();
        const size_t total_len = contents.size();

        while (total_written < total_len) {
          const DWORD chunk = static_cast<DWORD>(std::min<size_t>(total_len - total_written, MAXDWORD));
          if (!WriteFile(h_file, data_ptr + total_written, chunk, &written, nullptr)) {
            BOOST_LOG(error) << "WriteFile failed writing to " << temp_path << ": " << GetLastError();
            return -1;
          }
          if (written == 0) {
            BOOST_LOG(error) << "WriteFile wrote 0 bytes to " << temp_path;
            return -1;
          }
          total_written += written;
        }
      }

      if (!FlushFileBuffers(h_file)) {
        BOOST_LOG(error) << "FlushFileBuffers failed on " << temp_path << ": " << GetLastError();
        return -1;
      }

      if (!handle_guard.close()) {
        BOOST_LOG(error) << "CloseHandle failed before atomic replacement: " << GetLastError();
        return -1;
      }

#ifdef SUNSHINE_TESTS
      if (test_support::get_failure_injection() == test_support::FailStage::BeforeReplacement) {
        BOOST_LOG(error) << "Injected failure before replacement for " << path;
        return -1;
      }
#endif

      // Atomic rename on the same volume. NO MOVEFILE_COPY_ALLOWED, no target deletion first.
      if (!MoveFileExW(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        BOOST_LOG(error) << "MoveFileExW failed replacing " << path << " with " << temp_path << ": " << GetLastError();
        return -1;
      }

      temp_guard.disarm();
      return 0;
#else
      mode_t target_mode = S_IRUSR | S_IWUSR;
      bool target_exists = false;

      struct stat st_target;
      if (lstat(path.c_str(), &st_target) == 0) {
        if (S_ISDIR(st_target.st_mode)) {
          BOOST_LOG(error) << "Target is a directory, refusing atomic write: " << path;
          return -1;
        }
        if (S_ISLNK(st_target.st_mode)) {
          BOOST_LOG(error) << "Target is a symlink, refusing atomic write: " << path;
          return -1;
        }
        if (!S_ISREG(st_target.st_mode)) {
          BOOST_LOG(error) << "Target is not a regular file, refusing atomic write: " << path;
          return -1;
        }
        target_exists = true;
        target_mode = st_target.st_mode & 0777;
      } else if (errno != ENOENT) {
        BOOST_LOG(error) << "lstat failed on target " << path << ": " << errno;
        return -1;
      }

      // Initial temporary permissions are strictly 0600 (S_IRUSR | S_IWUSR) to avoid exposing staged data
      int fd = -1;
      for (int attempt = 0; attempt < 50; ++attempt) {
        const auto rand_val = dis(gen);
        const auto temp_name = std::format("{}.tmp.{:x}.{:x}", path.filename().native(), current_count, rand_val);
        temp_path = parent_dir.empty() ? std::filesystem::path(temp_name) : (parent_dir / temp_name);

        fd = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (fd >= 0) {
          break;
        }

        if (errno != EEXIST) {
          BOOST_LOG(error) << "open failed creating temp file " << temp_path << ": " << errno;
          return -1;
        }
      }

      if (fd < 0) {
        BOOST_LOG(error) << "Failed to exclusively create unique temp file after retries for " << path;
        return -1;
      }

      TempCleanupGuard temp_guard {temp_path};
      FdGuard fd_guard {fd};

#ifdef SUNSHINE_TESTS
      if (test_support::get_failure_injection() == test_support::FailStage::AfterPartialWrite) {
        const size_t partial_len = contents.size() > 1 ? (contents.size() / 2) : (contents.empty() ? 0 : 1);
        if (partial_len > 0) {
          [[maybe_unused]] auto res = write(fd, contents.data(), partial_len);
        }
        fd_guard.close();
        return -1;
      }
#endif

      size_t total_written = 0;
      const char *data_ptr = contents.data();
      const size_t total_len = contents.size();

      while (total_written < total_len) {
        const ssize_t res = write(fd, data_ptr + total_written, total_len - total_written);
        if (res < 0) {
          if (errno == EINTR) {
            continue;
          }
          BOOST_LOG(error) << "write failed writing to " << temp_path << ": " << errno;
          return -1;
        }
        if (res == 0) {
          BOOST_LOG(error) << "write returned 0 bytes writing to " << temp_path;
          return -1;
        }
        total_written += static_cast<size_t>(res);
      }

      while (fsync(fd) != 0) {
        if (errno == EINTR) {
          continue;
        }
        BOOST_LOG(error) << "fsync failed on temp file " << temp_path << ": " << errno;
        return -1;
      }

      if (target_exists) {
        if (fchmod(fd, target_mode) != 0) {
          BOOST_LOG(error) << "fchmod failed to preserve target mode on " << temp_path << ": " << errno;
          return -1;
        }
      }

      if (!fd_guard.close()) {
        BOOST_LOG(error) << "close failed before atomic replacement: " << errno;
        return -1;
      }

#ifdef SUNSHINE_TESTS
      if (test_support::get_failure_injection() == test_support::FailStage::BeforeReplacement) {
        BOOST_LOG(error) << "Injected failure before replacement for " << path;
        return -1;
      }
#endif

      if (rename(temp_path.c_str(), path.c_str()) != 0) {
        BOOST_LOG(error) << "rename failed from " << temp_path << " to " << path << ": " << errno;
        return -1;
      }

      if (!parent_dir.empty()) {
        int dir_fd = open(parent_dir.c_str(), O_RDONLY | O_CLOEXEC);
        if (dir_fd >= 0) {
          fsync(dir_fd);
          close(dir_fd);
        }
      }

      temp_guard.disarm();
      return 0;
#endif
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Exception in write_file_atomic for " << path << ": " << e.what();
      return -1;
    } catch (...) {
      BOOST_LOG(error) << "Unknown exception in write_file_atomic for " << path;
      return -1;
    }
  }
}  // namespace file_handler

/**
 * @file file_handler.h
 * @brief Declarations for file handling functions.
 */
#pragma once

// standard includes
#include <filesystem>
#include <string>
#include <string_view>

/**
 * @brief Responsible for file handling functions.
 */
namespace file_handler {
  /**
   * @brief Get the parent directory of a file or directory.
   * @param path The path of the file or directory.
   * @return The parent directory.
   * @examples
   * std::string parent_dir = get_parent_directory("path/to/file");
   * @examples_end
   */
  std::string get_parent_directory(const std::string &path);

  /**
   * @brief Make a directory.
   * @param path The path of the directory.
   * @return `true` on success, `false` on failure.
   * @examples
   * bool dir_created = make_directory("path/to/directory");
   * @examples_end
   */
  bool make_directory(const std::string &path);

  /**
   * @brief Read a file to string.
   * @param path The path of the file.
   * @return The contents of the file.
   * @examples
   * std::string contents = read_file("path/to/file");
   * @examples_end
   */
  std::string read_file(const char *path);

  /**
   * @brief Read a file to string from a filesystem path.
   * @param path The filesystem path of the file.
   * @return The contents of the file.
   * @examples
   * std::string contents = read_file(std::filesystem::path("path/to/file"));
   * @examples_end
   */
  std::string read_file(const std::filesystem::path &path);

  /**
   * @brief Writes a file.
   * @param path The path of the file.
   * @param contents The contents to write.
   * @return ``0`` on success, ``-1`` on failure.
   * @examples
   * int write_status = write_file("path/to/file", "file contents");
   * @examples_end
   */
  int write_file(const char *path, const std::string_view &contents);

  /**
   * @brief Atomically write contents to a destination file.
   *
   * Writes contents to a unique temporary file in the destination's parent directory,
   * flushes data to storage, closes the file, and replaces the target atomically using
   * a robust same-volume rename strategy:
   * - On Windows: uses MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
   *   without cross-volume copy (MOVEFILE_COPY_ALLOWED is prohibited). Preserves target DACL
   *   (including protected/inheritance flags) on the staged temporary file at CreateFileW
   *   prior to writing payload bytes to prevent intermediate exposure via broader inherited
   *   ACLs. New targets follow parent directory inheritance. Note: file streams and creation
   *   timestamps are not preserved by rename.
   * - On POSIX: creates temporary file with restrictive permissions (0600) via O_CREAT | O_EXCL,
   *   flushes with fsync, preserves target permission mode prior to commit via fchmod
   *   (subject to umask/ownership limits), and renames atomically via rename().
   *
   * Validates that an existing destination is a regular non-reparse, non-symlink, non-directory
   * file, failing closed on unexpected file types. Exception-safe RAII guards ensure file
   * descriptors and temporary files are cleaned up without throwing.
   *
   * @param path The path of the destination file.
   * @param contents The contents to write.
   * @return `0` on success, `-1` on failure.
   * @examples
   * int status = write_file_atomic("path/to/file", "file contents");
   * @examples_end
   */
  int write_file_atomic(const std::filesystem::path &path, const std::string_view &contents);

#ifdef SUNSHINE_TESTS
  namespace test_support {
    /**
     * @brief Stages for controlled failure injection in write_file_atomic.
     */
    enum class FailStage {
      None,               ///< No failure injected.
      AfterPartialWrite,  ///< Inject failure after partial write to temporary file.
      BeforeReplacement   ///< Inject failure after temporary file write/flush but before atomic replacement.
    };

    /**
     * @brief Set the current failure injection stage for testing.
     *
     * @param stage The failure stage to inject.
     */
    void set_failure_injection(FailStage stage);

    /**
     * @brief Reset failure injection to default (None).
     */
    void reset_failure_injection();

    /**
     * @brief Get the current failure injection stage.
     *
     * @return The currently active failure stage.
     */
    FailStage get_failure_injection();
  }  // namespace test_support
#endif
}  // namespace file_handler

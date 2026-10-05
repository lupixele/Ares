/**
 * @file tests/unit/test_file_handler.cpp
 * @brief Test src/file_handler.*.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <format>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <sddl.h>
#endif

// local includes
#include <src/file_handler.h>

struct FileHandlerParentDirectoryTest: BaseTest, testing::WithParamInterface<std::tuple<std::string, std::string>> {};

TEST_P(FileHandlerParentDirectoryTest, Run) {
  auto [input, expected] = GetParam();
  EXPECT_EQ(file_handler::get_parent_directory(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  FileHandlerTests,
  FileHandlerParentDirectoryTest,
  testing::Values(
    std::make_tuple("/path/to/file.txt", "/path/to"),
    std::make_tuple("/path/to/directory", "/path/to"),
    std::make_tuple("/path/to/directory/", "/path/to")
  )
);

struct FileHandlerMakeDirectoryTest: BaseTest, testing::WithParamInterface<std::tuple<std::string, bool, bool>> {};

TEST_P(FileHandlerMakeDirectoryTest, Run) {
  auto [input, expected, remove] = GetParam();
  const std::string test_dir = platf::appdata().string() + "/tests/path/";
  input = test_dir + input;

  EXPECT_EQ(file_handler::make_directory(input), expected);
  EXPECT_TRUE(std::filesystem::exists(input));

  // remove test directory
  if (remove) {
    std::filesystem::remove_all(test_dir);
    EXPECT_FALSE(std::filesystem::exists(test_dir));
  }
}

INSTANTIATE_TEST_SUITE_P(
  FileHandlerTests,
  FileHandlerMakeDirectoryTest,
  testing::Values(
    std::make_tuple("dir_123", true, false),
    std::make_tuple("dir_123", true, true),
    std::make_tuple("dir_123/abc", true, false),
    std::make_tuple("dir_123/abc", true, true)
  )
);

struct FileHandlerTests: BaseTest, testing::WithParamInterface<std::tuple<int, std::string>> {};

INSTANTIATE_TEST_SUITE_P(
  TestFiles,
  FileHandlerTests,
  testing::Values(
    std::make_tuple(0, ""),  // empty file
    std::make_tuple(1, "a"),  // single character
    std::make_tuple(2, "Mr. Blue Sky - Electric Light Orchestra"),  // single line
    std::make_tuple(3, R"(
Morning! Today's forecast calls for blue skies
The sun is shining in the sky
There ain't a cloud in sight
It's stopped raining
Everybody's in the play
And don't you know, it's a beautiful new day
Hey, hey, hey!
Running down the avenue
See how the sun shines brightly in the city
All the streets where once was pity
Mr. Blue Sky is living here today!
Hey, hey, hey!
    )")  // multi-line
  )
);

TEST_P(FileHandlerTests, WriteFileTest) {
  auto [fileNum, content] = GetParam();
  const std::string fileName = std::format("write_file_test_{}.txt", fileNum);
  EXPECT_EQ(file_handler::write_file(fileName.c_str(), content), 0);
}

TEST_P(FileHandlerTests, ReadFileTest) {
  auto [fileNum, content] = GetParam();
  const std::string fileName = std::format("write_file_test_{}.txt", fileNum);
  EXPECT_EQ(file_handler::read_file(fileName.c_str()), content);
}

TEST(FileHandlerTests, ReadMissingFileTest) {
  // read missing file
  EXPECT_EQ(file_handler::read_file("non-existing-file.txt"), "");
}

/**
 * @brief Test fixture for atomic file writes and failure injection.
 */
class FileHandlerAtomicWriteTest: public BaseTest {
protected:
  /**
   * @brief Set up isolated test directory and reset failure injection.
   */
  void SetUp() override {
    BaseTest::SetUp();
    test_dir = std::filesystem::path(SUNSHINE_TEST_BIN_DIR) / "test_file_handler_atomic";
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);
    file_handler::test_support::reset_failure_injection();
  }

  /**
   * @brief Clean up isolated test directory and reset failure injection.
   */
  void TearDown() override {
    file_handler::test_support::reset_failure_injection();
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    BaseTest::TearDown();
  }

  /**
   * @brief Check whether any temporary files exist in the test directory.
   *
   * @return True if any temporary files exist, false otherwise.
   */
  [[nodiscard]] bool has_temp_files() const {
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(test_dir, ec)) {
#ifdef _WIN32
      if (entry.path().filename().native().find(L".tmp.") != std::wstring::npos) {
        return true;
      }
#else
      if (entry.path().filename().native().find(".tmp.") != std::string::npos) {
        return true;
      }
#endif
    }
    return false;
  }

  std::filesystem::path test_dir;  ///< Isolated temporary directory for atomic write tests.
};

TEST_F(FileHandlerAtomicWriteTest, NonexistentTargetSuccess) {
  const auto target = test_dir / "new_target.json";
  const std::string payload = R"({"status":"created","count":42})";

  EXPECT_EQ(file_handler::write_file_atomic(target, payload), 0);
  EXPECT_TRUE(std::filesystem::exists(target));
  EXPECT_EQ(file_handler::read_file(target.string().c_str()), payload);
  EXPECT_FALSE(has_temp_files());
}

TEST_F(FileHandlerAtomicWriteTest, ReplaceExistingTargetSuccess) {
  const auto target = test_dir / "existing_target.json";
  const std::string initial = R"({"version":1,"client":"old"})";
  const std::string updated = R"({"version":2,"client":"new"})";

  EXPECT_EQ(file_handler::write_file_atomic(target, initial), 0);
  EXPECT_EQ(file_handler::read_file(target.string().c_str()), initial);

  EXPECT_EQ(file_handler::write_file_atomic(target, updated), 0);
  EXPECT_EQ(file_handler::read_file(target.string().c_str()), updated);
  EXPECT_FALSE(has_temp_files());
}

TEST_F(FileHandlerAtomicWriteTest, FailureAfterPartialWritePreservesExistingBytesAndCleansTemp) {
  const auto target = test_dir / "preserve_existing.json";
  const std::string original = R"({"critical":"unaltered_state_bytes"})";
  const std::string attempted = R"({"critical":"corrupted_state_bytes"})";

  EXPECT_EQ(file_handler::write_file_atomic(target, original), 0);
  EXPECT_EQ(file_handler::read_file(target.string().c_str()), original);

  file_handler::test_support::set_failure_injection(file_handler::test_support::FailStage::AfterPartialWrite);
  EXPECT_NE(file_handler::write_file_atomic(target, attempted), 0);
  file_handler::test_support::reset_failure_injection();

  // Existing bytes MUST remain completely unaltered
  EXPECT_EQ(file_handler::read_file(target.string().c_str()), original);
  // Own temporary file MUST be cleaned up
  EXPECT_FALSE(has_temp_files());
}

TEST_F(FileHandlerAtomicWriteTest, FailureBeforeReplacementPreservesExistingBytesAndCleansTemp) {
  const auto target = test_dir / "preserve_existing_replace.json";
  const std::string original = R"({"root":{"uniqueid":"preserved"}})";
  const std::string attempted = R"({"root":{"uniqueid":"overwritten"}})";

  EXPECT_EQ(file_handler::write_file_atomic(target, original), 0);
  EXPECT_EQ(file_handler::read_file(target.string().c_str()), original);

  file_handler::test_support::set_failure_injection(file_handler::test_support::FailStage::BeforeReplacement);
  EXPECT_NE(file_handler::write_file_atomic(target, attempted), 0);
  file_handler::test_support::reset_failure_injection();

  // Existing target bytes MUST remain intact
  EXPECT_EQ(file_handler::read_file(target.string().c_str()), original);
  // Temporary file MUST be cleaned up, never leaving orphan files
  EXPECT_FALSE(has_temp_files());
}

TEST_F(FileHandlerAtomicWriteTest, NonexistentTargetFailureInjectionCleansTemp) {
  const auto target = test_dir / "should_not_exist.json";
  const std::string attempted = R"({"data":"temporary"})";

  file_handler::test_support::set_failure_injection(file_handler::test_support::FailStage::AfterPartialWrite);
  EXPECT_NE(file_handler::write_file_atomic(target, attempted), 0);
  file_handler::test_support::reset_failure_injection();

  EXPECT_FALSE(std::filesystem::exists(target));
  EXPECT_FALSE(has_temp_files());
}

TEST_F(FileHandlerAtomicWriteTest, InvalidDestinationPathFailsGracefullyWithoutSideEffects) {
  const auto invalid_target = test_dir / "nonexistent_subdirectory_for_atomic_test" / "state.json";
  const std::string attempted = R"({"status":"fail"})";

  EXPECT_NE(file_handler::write_file_atomic(invalid_target, attempted), 0);
  EXPECT_FALSE(std::filesystem::exists(invalid_target));
}

TEST_F(FileHandlerAtomicWriteTest, EmptyContentsSuccess) {
  const auto target = test_dir / "empty_target.json";

  // Test creating new file with empty contents
  EXPECT_EQ(file_handler::write_file_atomic(target, ""), 0);
  EXPECT_TRUE(std::filesystem::exists(target));
  EXPECT_EQ(file_handler::read_file(target.string().c_str()), "");
  EXPECT_EQ(std::filesystem::file_size(target), 0);
  EXPECT_FALSE(has_temp_files());

  // Test replacing existing file with non-empty then back to empty
  const std::string non_empty = "some data";
  EXPECT_EQ(file_handler::write_file_atomic(target, non_empty), 0);
  EXPECT_EQ(file_handler::read_file(target.string().c_str()), non_empty);

  EXPECT_EQ(file_handler::write_file_atomic(target, ""), 0);
  EXPECT_EQ(file_handler::read_file(target.string().c_str()), "");
  EXPECT_EQ(std::filesystem::file_size(target), 0);
  EXPECT_FALSE(has_temp_files());
}

TEST_F(FileHandlerAtomicWriteTest, UnicodeFilenameSuccess) {
  const auto target = test_dir / u8"клиент_日本語_🚀_état.json";
  const std::string payload = R"({"unicode":"success_данные_キー"})";

  EXPECT_EQ(file_handler::write_file_atomic(target, payload), 0);
  EXPECT_TRUE(std::filesystem::exists(target));
  EXPECT_EQ(file_handler::read_file(target), payload);
  EXPECT_FALSE(has_temp_files());
}

TEST_F(FileHandlerAtomicWriteTest, TargetIsDirectoryFailsClosedWithoutModifying) {
  const auto dir_target = test_dir / "target_directory";
  std::error_code ec;
  std::filesystem::create_directory(dir_target, ec);
  ASSERT_TRUE(std::filesystem::is_directory(dir_target));

  const std::string payload = "forbidden directory overwrite";
  EXPECT_NE(file_handler::write_file_atomic(dir_target, payload), 0);

  // Directory must remain intact and not replaced
  EXPECT_TRUE(std::filesystem::is_directory(dir_target));
  EXPECT_FALSE(has_temp_files());
}

#ifdef _WIN32
TEST_F(FileHandlerAtomicWriteTest, FailedRealRenameReadLockPreservesOriginalBytesAndCleansTemp) {
  const auto target = test_dir / "locked_target.txt";
  const std::string original = "IMMUTABLE_ORIGINAL_BYTES";
  const std::string attempted = "ATTEMPTED_OVERWRITE_MUTATION";

  EXPECT_EQ(file_handler::write_file_atomic(target, original), 0);
  EXPECT_EQ(file_handler::read_file(target.string().c_str()), original);

  // Open the target file sharing read only (omitting FILE_SHARE_DELETE).
  // This causes MoveFileExW to fail with a real Windows sharing violation (ERROR_ACCESS_DENIED/SHARING_VIOLATION).
  HANDLE h_locked = CreateFileW(
    target.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr
  );
  ASSERT_NE(h_locked, INVALID_HANDLE_VALUE);

  EXPECT_NE(file_handler::write_file_atomic(target, attempted), 0);

  CloseHandle(h_locked);

  // Original target bytes must remain completely intact despite real OS rename failure
  EXPECT_EQ(file_handler::read_file(target.string().c_str()), original);
  // Staged temporary file must be cleaned up
  EXPECT_FALSE(has_temp_files());
}

TEST_F(FileHandlerAtomicWriteTest, RestrictiveDaclPreservedRoundtripAndNotBroader) {
  const auto target = test_dir / "restrictive_dacl.json";
  const std::string initial = R"({"security":"initial"})";
  const std::string updated = R"({"security":"updated_secret"})";

  EXPECT_EQ(file_handler::write_file_atomic(target, initial), 0);

  // Apply a custom protected DACL (owner full control, no inheritance) to fixture file
  PSECURITY_DESCRIPTOR p_custom_sd = nullptr;
  ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
    L"D:P(A;;FA;;;OW)",
    SDDL_REVISION_1,
    &p_custom_sd,
    nullptr
  ));

  BOOL set_sec = SetFileSecurityW(
    target.c_str(),
    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
    p_custom_sd
  );
  LocalFree(p_custom_sd);
  ASSERT_TRUE(set_sec);

  // Query SDDL before atomic write
  const SECURITY_INFORMATION sec_info = DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION;
  DWORD needed1 = 0;
  GetFileSecurityW(target.c_str(), sec_info, nullptr, 0, &needed1);
  ASSERT_GT(needed1, 0u);
  std::vector<BYTE> buf1(needed1);
  ASSERT_TRUE(GetFileSecurityW(target.c_str(), sec_info, reinterpret_cast<PSECURITY_DESCRIPTOR>(buf1.data()), needed1, &needed1));

  LPWSTR sddl_before = nullptr;
  ASSERT_TRUE(ConvertSecurityDescriptorToStringSecurityDescriptorW(
    reinterpret_cast<PSECURITY_DESCRIPTOR>(buf1.data()),
    SDDL_REVISION_1,
    sec_info,
    &sddl_before,
    nullptr
  ));

  // Perform atomic replacement
  EXPECT_EQ(file_handler::write_file_atomic(target, updated), 0);
  EXPECT_EQ(file_handler::read_file(target.string().c_str()), updated);

  // Query SDDL after atomic write
  DWORD needed2 = 0;
  GetFileSecurityW(target.c_str(), sec_info, nullptr, 0, &needed2);
  ASSERT_GT(needed2, 0u);
  std::vector<BYTE> buf2(needed2);
  ASSERT_TRUE(GetFileSecurityW(target.c_str(), sec_info, reinterpret_cast<PSECURITY_DESCRIPTOR>(buf2.data()), needed2, &needed2));

  LPWSTR sddl_after = nullptr;
  ASSERT_TRUE(ConvertSecurityDescriptorToStringSecurityDescriptorW(
    reinterpret_cast<PSECURITY_DESCRIPTOR>(buf2.data()),
    SDDL_REVISION_1,
    sec_info,
    &sddl_after,
    nullptr
  ));

  std::wstring before_str(sddl_before);
  std::wstring after_str(sddl_after);
  LocalFree(sddl_before);
  LocalFree(sddl_after);

  // Verify that restrictive DACL is preserved and not broadened
  EXPECT_EQ(before_str, after_str);
  EXPECT_FALSE(has_temp_files());
}
#endif

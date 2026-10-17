On Windows we use msys2 and ucrt64 to compile.
You need to prefix commands with `P:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.

Prefix build directories with `cmake-build-`.

The test executable is named `test_sunshine` and will be located inside the `tests` directory within
the build directory.

The project uses gtest as a test framework.

## Local build notes (dev machine)

MSYS2 is installed on `P:` here, not `C:`, despite the upstream convention. Two further quirks on this
machine make a plain `msys2_shell.cmd -c` invocation fail; use `cmake-build-run.sh` in the worktree root,
which sets everything up before exec'ing your command:

```powershell
& 'P:\msys64\usr\bin\bash.exe' -l 'P:\Magnanimity\Projects\Aresthena\Ares\.migration-worktrees\sunshine\cmake-build-run.sh' <command>
```

- **git rejects the source tree.** Native Windows git reports `dubious ownership` for the `P:\` path
  unless `HOME` points at the real user profile; the MSYS login shell resets it. The wrapper exports
  `HOME=C:\Users\lupixele`.
- **git must be first on PATH.** CMake hands MSYS-style paths to native git during configure, so
  `P:\Program Files\Git\cmd` is prepended.
- **Build with `-j1`.** 24 GB of RAM but a small commit limit: parallel jobs exhaust the process
  address space and fail *unrelated* translation units with `error: out of memory`. Those failures are
  not code defects. Grow the page file before raising `-j` above 1.

## Testing against a real virtual display

`SudovdaDriverTest.OpensInstalledDriverWhenPresent` is the only case that reaches real hardware. It
performs a read-only handshake (open, protocol version, watchdog ping) and no allocation, and it skips
cleanly when no SudoVDA control device is present, so the suite stays green on hosts without the driver.


When adding localization do not update any language other than `en`. This also means to exclude en-US or other variants.

Always add or update doxygen documentation.

The project requires that everything be documented in doxygen or the build will fail.

Primary doxygen comments should be done like so:

```cpp
  /**
   * @brief Describe the function, structure, etc.
   *
   * @param my_param Describe the parameter.
   * @return Describe the return.
   */
```

Inline doxygen comments should use `///< ...` instead of `/**< ... */`.

Always follow the style guidelines defined in .clang-format for c/c++ code.

Do not ever create issues or pull requests.
If asked to create an issue or pull request, do so in their fork instead of the LizardByte GitHub organization.
Never create an issue or pull request in the LizardByte GitHub organization.

Add or update tests for new or modified methods and code. Target 100% coverage on changed code.

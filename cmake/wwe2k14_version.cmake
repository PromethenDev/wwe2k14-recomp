# Player-facing release version, shared by the launcher (and its auto-updater) and tools/package-release.sh.
# The single source is the VERSION file at the repository root (one line, e.g. "1.1.0"); bump it
# for every release. WWE2K14_VERSION_COMMIT is the short git hash of the tree being built, with a
# "+dirty" suffix for uncommitted changes, so two builds of the same version can still be told apart.
get_filename_component(_wwe2k14_version_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_wwe2k14_version_root}/VERSION")
file(STRINGS "${_wwe2k14_version_root}/VERSION" WWE2K14_VERSION LIMIT_COUNT 1)
string(STRIP "${WWE2K14_VERSION}" WWE2K14_VERSION)
if(NOT WWE2K14_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.]+)?$")
  message(FATAL_ERROR "VERSION must look like 1.1.0 or 1.1.0-rc1, got '${WWE2K14_VERSION}'")
endif()

set(WWE2K14_VERSION_COMMIT unknown)
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
    WORKING_DIRECTORY "${_wwe2k14_version_root}"
    OUTPUT_VARIABLE _wwe2k14_version_hash OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _wwe2k14_version_result ERROR_QUIET)
  if(_wwe2k14_version_result EQUAL 0)
    set(WWE2K14_VERSION_COMMIT "${_wwe2k14_version_hash}")
    # Re-run configure after every commit/checkout (the reflog) or staging change (the index), so
    # an incremental build never shows a stale hash.
    foreach(_wwe2k14_git_file logs/HEAD index)
      execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute --git-path "${_wwe2k14_git_file}"
        WORKING_DIRECTORY "${_wwe2k14_version_root}"
        OUTPUT_VARIABLE _wwe2k14_git_path OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      if(EXISTS "${_wwe2k14_git_path}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_wwe2k14_git_path}")
      endif()
    endforeach()
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
      WORKING_DIRECTORY "${_wwe2k14_version_root}"
      OUTPUT_VARIABLE _wwe2k14_version_status OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _wwe2k14_version_status STREQUAL "")
      string(APPEND WWE2K14_VERSION_COMMIT "+dirty")
    endif()
  endif()
endif()

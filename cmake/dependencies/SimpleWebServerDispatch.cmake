# Build-tree overlay only: never mutate or repoint the pinned submodule.
set(_sws_source "${CMAKE_SOURCE_DIR}/third-party/Simple-Web-Server")
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_sws_source}" rev-parse HEAD
    OUTPUT_VARIABLE _sws_revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT _sws_revision STREQUAL "546895a93a29062bb178367b46c7afb72da9881e")
    message(FATAL_ERROR "Simple-Web-Server dispatch overlay requires pinned revision 546895a93a29062bb178367b46c7afb72da9881e")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_sws_source}" diff --quiet HEAD -- .
    RESULT_VARIABLE _sws_dirty)
if(NOT _sws_dirty EQUAL 0)
    message(FATAL_ERROR "Simple-Web-Server overlay requires a pristine pinned submodule")
endif()
file(READ "${_sws_source}/server_http.hpp" _sws_original)
string(REPLACE "\r\n" "\n" _sws_original "${_sws_original}")
string(SHA256 _sws_hash "${_sws_original}")
if(NOT _sws_hash STREQUAL "60381deda39ba9c9aa5b7d5b864b7e73370b3e0699b4fe0fafb43402efe37795")
    message(FATAL_ERROR "Simple-Web-Server server_http.hpp differs from reviewed pinned bytes")
endif()
set(_sws_overlay "${CMAKE_BINARY_DIR}/dependency-overlays/Simple-Web-Server")
file(MAKE_DIRECTORY "${_sws_overlay}")
file(GLOB _sws_headers "${_sws_source}/*.hpp")
foreach(_sws_header IN LISTS _sws_headers)
    configure_file("${_sws_header}" "${_sws_overlay}/" COPYONLY)
endforeach()
configure_file("${_sws_source}/LICENSE" "${_sws_overlay}/LICENSE" COPYONLY)
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --unsafe-paths "--directory=${_sws_overlay}" --check "${CMAKE_CURRENT_LIST_DIR}/patches/simple-web-server-dispatch.patch"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --unsafe-paths "--directory=${_sws_overlay}" "${CMAKE_CURRENT_LIST_DIR}/patches/simple-web-server-dispatch.patch"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
file(READ "${_sws_overlay}/server_http.hpp" _sws_result)
if(NOT _sws_result MATCHES "virtual bool authorize_request")
    message(FATAL_ERROR "Simple-Web-Server dispatch patch was not applied")
endif()
include_directories(BEFORE "${CMAKE_BINARY_DIR}/dependency-overlays")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/patches/simple-web-server-dispatch.patch"
    "${_sws_source}/server_http.hpp")

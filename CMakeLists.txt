cmake_minimum_required(VERSION 3.14)
project(LinuxCamPAM VERSION 0.9.7.2 LANGUAGES C CXX)

# --- Automatic Git Hook Installation ---
# Installs/Updates the pre-push hook to prevent accidental pushes to master
if(EXISTS "${CMAKE_SOURCE_DIR}/.git" AND EXISTS "${CMAKE_SOURCE_DIR}/scripts/hooks/pre-push")
    message(STATUS "Installing git pre-push hook...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/scripts/hooks/pre-push"
        "${CMAKE_SOURCE_DIR}/.git/hooks/pre-push"
    )
    # Ensure it's executable
    execute_process(
        COMMAND chmod +x "${CMAKE_SOURCE_DIR}/.git/hooks/pre-push"
    )
endif()

# --- Versioning with Git Hash ---
find_package(Git)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

if(GIT_HASH)
    set(FULL_VERSION "${PROJECT_VERSION}+g${GIT_HASH}")
else()
    string(TIMESTAMP BUILD_DATE "%Y%m%d")
    set(FULL_VERSION "${PROJECT_VERSION}+dev${BUILD_DATE}")
endif()

add_compile_definitions(LINUXCAMPAM_VERSION="${FULL_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Security Hardening ---
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Werror
        -fstack-protector-strong
        -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
        -fPIE
        -Wformat -Wformat-security
        -Werror=format-security
    )
    add_link_options(
        -Wl,-z,relro,-z,now
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:-pie>
    )
endif()

# --- Dependencies ---
# Prioritize local static build of OpenCV 4.12+
set(LOCAL_OPENCV "${CMAKE_SOURCE_DIR}/opencv_static")
if(EXISTS "${LOCAL_OPENCV}/lib/cmake/opencv4/OpenCVConfig.cmake")
    message(STATUS "Using Local Static OpenCV from: ${LOCAL_OPENCV}")
    set(OpenCV_DIR "${LOCAL_OPENCV}/lib/cmake/opencv4")
    set(OpenCV_STATIC ON)
else()
    message(WARNING "Local OpenCV not found at ${LOCAL_OPENCV}. Falling back to system OpenCV (might cause incompatibility).")
endif()
find_package(OpenCV REQUIRED COMPONENTS core videoio imgproc dnn objdetect photo)
find_package(Threads REQUIRED)

# PAM (Usually header-only or simple lib lookup)
find_library(PAM_LIBRARY pam)
find_path(PAM_INCLUDE_DIR security/pam_appl.h)
if(NOT PAM_LIBRARY OR NOT PAM_INCLUDE_DIR)
    message(FATAL_ERROR "PAM library or headers not found. Install libpam0g-dev.")
endif()

# ONNX Runtime finding removed as we use OpenCV DNN


# JSON (nlohmann/json) - use system if available, else vendored
find_package(nlohmann_json QUIET)

# --- Configuration ---
configure_file(${CMAKE_SOURCE_DIR}/config/config.ini ${CMAKE_BINARY_DIR}/config.ini COPYONLY)

# --- Targets ---

# PAM Module
add_library(pam_linuxcampam SHARED src/pam/pam_linuxcampam.cpp)
target_include_directories(pam_linuxcampam PRIVATE include src/common ${PAM_INCLUDE_DIR})
target_link_libraries(pam_linuxcampam PRIVATE ${PAM_LIBRARY})
set_target_properties(pam_linuxcampam PROPERTIES PREFIX "") # pam_ modules often don't want strict lib prefix, though standard is usually ok. PAM expects filename to match config.

# Service Core Library (Shared between Service and Tests)
add_library(service_core STATIC
    src/service/auth_engine.cpp
    src/service/camera.cpp
    src/service/config.cpp
    src/service/utils.cpp
)
target_include_directories(service_core PUBLIC include src src/common ${OpenCV_INCLUDE_DIRS})
target_link_libraries(service_core PUBLIC 
    ${OpenCV_LIBS} 
    Threads::Threads
    stdc++fs
)

if(nlohmann_json_FOUND)
    target_link_libraries(service_core PUBLIC nlohmann_json::nlohmann_json)
endif()

# Auth Service
add_executable(linuxcampamd src/service/main.cpp)
target_link_libraries(linuxcampamd PRIVATE service_core)

# CLI Tool
add_executable(linuxcampam src/cli/main.cpp)
target_include_directories(linuxcampam PRIVATE include)
target_link_libraries(linuxcampam PRIVATE stdc++fs)

# Diagnostic Tool
add_executable(check_opencl src/tools/check_opencl.cpp)
target_link_libraries(check_opencl PRIVATE ${OpenCV_LIBS})

# --- Testing ---
# --- Testing ---
enable_testing()

include(FetchContent)
# Handle DOWNLOAD_EXTRACT_TIMESTAMP for CMake 3.24+
set(GTEST_DOWNLOAD_ARGS "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
  set(GTEST_DOWNLOAD_ARGS DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  ${GTEST_DOWNLOAD_ARGS}
)

# Fix for CMP0135 warning in CMake 3.24+
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
  # CMake 3.24+ requires explicit timestamp handling for consistency
  set(FETCHCONTENT_UPDATES_DISCONNECTED_GOOGLETEST ON)
endif()
# Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# Disable installation of GoogleTest to prevent conflicts with system packages
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(INSTALL_GMOCK OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

if(TARGET gtest)
    add_executable(linuxcampam_tests
        tests/main.cpp
        tests/test_auth_config.cpp
        tests/test_model_version.cpp
        tests/test_embeddings.cpp
        tests/test_security.cpp
        tests/test_stability.cpp
        tests/test_camera_detection.cpp
        tests/test_config_detection.cpp
        tests/test_auth_logic.cpp
        tests/test_protocol.cpp
        tests/test_pam_logic.cpp
    )
    
    # We link against the service_core library to test internal logic without 
    # duplicating compilation units or pulling in the main service entry point.
    target_link_libraries(linuxcampam_tests PRIVATE 
        gtest gtest_main gmock
        service_core
    )
    
    add_test(NAME UnitTests COMMAND linuxcampam_tests)
endif()

# --- Install Targets ---
include(GNUInstallDirs)

install(TARGETS linuxcampamd linuxcampam check_opencl
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS pam_linuxcampam
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/security
)

# --- Install Config and Models ---
install(FILES ${CMAKE_SOURCE_DIR}/config/config.ini
    DESTINATION /etc/linuxcampam
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/models/
    DESTINATION /usr/share/linuxcampam/models
    FILES_MATCHING PATTERN "*.onnx"
)

install(FILES ${CMAKE_SOURCE_DIR}/config/linuxcampam_pam_profile
    DESTINATION /usr/share/pam-configs
    RENAME linuxcampam
)



install(FILES ${CMAKE_SOURCE_DIR}/scripts/linuxcampam.service
    DESTINATION /lib/systemd/system
)

install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/setup_config.sh
    DESTINATION bin
    RENAME linuxcampam-setup-config
)

install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/detect_opencl.sh
    DESTINATION bin
)

# --- CPack Packaging ---
set(CPACK_PACKAGE_NAME "linuxcampam")
set(CPACK_PACKAGE_VENDOR "Vladimir Orlinski")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Linux PAM Face Authentication Module using OpenCV DNN")
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "contact.github.submerge594@slmails.com")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Vladimir Orlinski <contact.github.submerge594@slmails.com>")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/Vladush/LinuxCamPAM")
set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")

set(CPACK_GENERATOR "DEB")
# Runtime dependencies (not -dev packages) for Ubuntu 24.04
# OpenCV is statically linked, so we don't need libopencv-* packages
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libpam0g, libpam-runtime, v4l-utils, systemd, libgfortran5, libatlas3-base")

set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_SOURCE_DIR}/debian/postinst")

include(CPack)

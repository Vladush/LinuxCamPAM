cmake_minimum_required(VERSION 3.14)
project(LinuxCamPAM VERSION 0.9.7.4 LANGUAGES C CXX)

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

option(DISABLE_WELCOME_MESSAGE "Disable PAM welcome message completely at compile time" OFF)
if(DISABLE_WELCOME_MESSAGE)
    add_compile_definitions(DISABLE_WELCOME_MESSAGE)
    message(STATUS "Compile-time option: PAM welcome message is DISABLED")
endif()

# --- Security Hardening ---
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Werror
        -fstack-protector-strong
        -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
        -Wformat -Wformat-security
        -Werror=format-security
    )
    # -fPIE is applied per-target to executables only (not the PAM .so module,
    # which needs pure -fPIC to avoid relocation errors with STL internals).
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

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(HIDAPI IMPORTED_TARGET hidapi-hidraw)
endif()

if(TARGET PkgConfig::HIDAPI)
    message(STATUS "Found system HIDAPI (hidraw)")
    add_library(hidapi_hidraw ALIAS PkgConfig::HIDAPI)
else()
    message(STATUS "System HIDAPI not found, fetching from GitHub...")
    include(FetchContent)
    set(HIDAPI_WITH_LIBUSB OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      hidapi
      URL https://github.com/libusb/hidapi/archive/refs/tags/hidapi-0.14.0.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(hidapi)
endif()

# ONNX Runtime finding removed in favor of OpenCV DNN


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
    src/PresenceTripwire.cpp
    src/VirtualKeyboard.cpp
    src/HardwareManager.cpp
)
target_include_directories(service_core PUBLIC include src src/common ${OpenCV_INCLUDE_DIRS})
target_link_libraries(service_core PUBLIC 
    ${OpenCV_LIBS} 
    Threads::Threads
    stdc++fs
    hidapi_hidraw
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

# Apply -fPIE to executables only (the PAM .so needs pure -fPIC).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(exe_target linuxcampamd linuxcampam check_opencl)
        target_compile_options(${exe_target} PRIVATE -fPIE)
    endforeach()
endif()

# --- Testing ---
# --- Testing ---
enable_testing()

include(FetchContent)
# Handle DOWNLOAD_EXTRACT_TIMESTAMP for CMake 3.24+
set(GTEST_DOWNLOAD_ARGS "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
  set(GTEST_DOWNLOAD_ARGS DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

# Try to find system-installed GTest and GMock first
find_package(GTest QUIET)

# Both GTest and GMock targets are required to use the system version
if(TARGET GTest::gtest AND TARGET GTest::gmock)
    message(STATUS "Found system GTest and GMock: ${GTest_VERSION}")
    
    if(NOT TARGET gtest)
        add_library(gtest ALIAS GTest::gtest)
    endif()
    if(NOT TARGET gtest_main)
        add_library(gtest_main ALIAS GTest::gtest_main)
    endif()
    if(NOT TARGET gmock)
        add_library(gmock ALIAS GTest::gmock)
    endif()
    if(NOT TARGET gmock_main AND TARGET GTest::gmock_main)
        add_library(gmock_main ALIAS GTest::gmock_main)
    endif()
else()
    message(STATUS "System GTest/GMock incomplete or missing, fetching from GitHub (archive)...")
    
    # Prevent GTest from installing its files during `cpack` or `make install`
    set(INSTALL_GTEST OFF CACHE BOOL "Disable installation of googletest" FORCE)
    set(INSTALL_GMOCK OFF CACHE BOOL "Disable installation of gmock" FORCE)
    
    # Use archive URL instead of git clone for better robustness against network/proxy issues
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
      URL_HASH SHA256=1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

if(TARGET gtest)
    add_executable(linuxcampam_tests
        tests/main.cpp
        tests/test_virtual_keyboard.cpp
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
        tests/test_config_parsing.cpp
        tests/test_camera.cpp
        tests/test_hardware_manager.cpp
        tests/test_presence_tripwire.cpp
        tests/test_ite8353_parser.cpp
        tests/test_sensor_factory.cpp
        tests/test_scoped_worker.cpp
    )
    
    # The tests link against the service_core library to test internal logic without 
    # duplicating compilation units or pulling in the main service entry point.
    target_link_libraries(linuxcampam_tests PRIVATE 
        gtest gtest_main gmock
        service_core
    )
    
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(linuxcampam_tests PRIVATE -fPIE)
    endif()
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
# OpenCV is statically linked, avoiding the need for libopencv-* packages
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libpam0g, libpam-runtime, v4l-utils, libgfortran5, libatlas3-base, libatomic1, libhidapi-hidraw0")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "rocm-opencl-runtime | intel-opencl-icd | nvidia-opencl-icd-340 | nvidia-opencl-icd")

set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_SOURCE_DIR}/debian/postinst")

# Man pages installation (dynamic generation)
find_program(PANDOC_EXECUTABLE pandoc)
if(PANDOC_EXECUTABLE)
    # Define custom target to generate man pages
    add_custom_target(generate_man_pages ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_SOURCE_DIR}/docs/man/generated
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_man_pages.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating man pages..."
    )

    # Note: installation is evaluated at CMake configure time, thus it must
    # specify the expected generated files explicitly since globs might be empty
    set(MAN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/docs/man/generated")
    install(FILES
        "${MAN_DIR}/linuxcampam.1"
        DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
        COMPONENT doc
    )
    install(FILES
        "${MAN_DIR}/linuxcampam.conf.5"
        DESTINATION "${CMAKE_INSTALL_MANDIR}/man5"
        COMPONENT doc
    )
    install(FILES
        "${MAN_DIR}/linuxcampamd.8"
        "${MAN_DIR}/pam_linuxcampam.8"
        DESTINATION "${CMAKE_INSTALL_MANDIR}/man8"
        COMPONENT doc
    )
else()
    message(WARNING "pandoc not found. Man pages will NOT be generated or installed. Please install pandoc.")
endif()

include(CPack)

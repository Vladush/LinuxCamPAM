cmake_minimum_required(VERSION 3.14)
project(LinuxCamPAM VERSION 0.9.3.3 LANGUAGES C CXX)

add_compile_definitions(LINUXCAMPAM_VERSION="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Security Hardening ---
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
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
target_include_directories(pam_linuxcampam PRIVATE include ${PAM_INCLUDE_DIR})
target_link_libraries(pam_linuxcampam PRIVATE ${PAM_LIBRARY})
set_target_properties(pam_linuxcampam PROPERTIES PREFIX "") # pam_ modules often don't want strict lib prefix, though standard is usually ok. PAM expects filename to match config.

# Auth Service
add_executable(linuxcampamd
    src/service/main.cpp
    src/service/auth_engine.cpp
    src/service/auth_engine.hpp
    src/service/camera.cpp
    src/service/camera.hpp
)
target_include_directories(linuxcampamd PRIVATE include ${OpenCV_INCLUDE_DIRS})
target_link_libraries(linuxcampamd PRIVATE 
    ${OpenCV_LIBS} 
    Threads::Threads
    stdc++fs
)

if(nlohmann_json_FOUND)
    target_link_libraries(linuxcampamd PRIVATE nlohmann_json::nlohmann_json)
endif()
# Always include local include dir for vendored headers
target_include_directories(linuxcampamd PRIVATE include)


# CLI Tool
add_executable(linuxcampam src/cli/main.cpp)
target_include_directories(linuxcampam PRIVATE include)
target_link_libraries(linuxcampam PRIVATE stdc++fs)

# Diagnostic Tool
add_executable(check_opencl src/tools/check_opencl.cpp)
target_link_libraries(check_opencl PRIVATE ${OpenCV_LIBS})

# --- Testing ---
enable_testing()
find_package(GTest)

if(GTest_FOUND)
    add_executable(linuxcampam_tests
        tests/main.cpp
        tests/test_config.cpp
        tests/test_model_version.cpp
        tests/test_embeddings.cpp
        tests/test_security.cpp
        src/service/auth_engine.cpp
        src/service/camera.cpp
    )
    # We need to compile auth_engine.cpp without main(), which is fine since main is in main.cpp.
    # However, auth_engine might have dependencies.
    target_include_directories(linuxcampam_tests PRIVATE include)
    target_link_libraries(linuxcampam_tests PRIVATE 
        GTest::GTest GTest::Main 
        ${OpenCV_LIBS} 
        Threads::Threads
        stdc++fs
    )
    if(nlohmann_json_FOUND)
        target_link_libraries(linuxcampam_tests PRIVATE nlohmann_json::nlohmann_json)
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

install(FILES ${CMAKE_SOURCE_DIR}/config/linuxcampam_pam_profile
    DESTINATION /usr/share/pam-configs
    RENAME linuxcampam
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/models
    DESTINATION /etc/linuxcampam
)

install(FILES ${CMAKE_SOURCE_DIR}/scripts/linuxcampam.service
    DESTINATION /lib/systemd/system
)

install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/setup_config.sh
    DESTINATION bin
    RENAME linuxcampam-setup-config
)

# --- CPack Packaging ---
set(CPACK_PACKAGE_NAME "linuxcampam")
set(CPACK_PACKAGE_VENDOR "Vladimir Orlinski")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Linux PAM Face Authentication Module")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "contact.github.submerge594@slmails.com")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Vladimir Orlinski")

set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libopencv-core406 | libopencv-core-dev, libopencv-imgproc406, libopencv-objdetect406, libopencv-dnn406, libpam0g, libc6, libstdc++6, v4l-utils")
# Note: Dependencies above are slightly generic, auto-shlibdeps is often handled by specialized toolchains, but CPack DEB needs manual or CMAKE_INSTALL_RPATH hint. 
# For Ubuntu 24.04, opencv might be 4.6+ or similar. "libopencv-dev" is strict.
# A safer minimal set:
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libopencv-dev (>= 4.0), libpam0g, libpam-runtime, v4l-utils, systemd, bash")

set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_SOURCE_DIR}/debian/postinst")

include(CPack)

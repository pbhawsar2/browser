cmake_minimum_required(VERSION 3.21)
project(explorer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(CTest)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(CRYPTO REQUIRED IMPORTED_TARGET libcrypto)

# Discover CEF binary distribution
if(NOT DEFINED CEF_ROOT OR "${CEF_ROOT}" STREQUAL "")
    if(DEFINED ENV{EXPLORER_CEF_DIR} AND EXISTS "$ENV{EXPLORER_CEF_DIR}/cmake/FindCEF.cmake")
        set(CEF_ROOT "$ENV{EXPLORER_CEF_DIR}")
    elseif(EXISTS "$ENV{HOME}/.local/share/explorer-cef/cmake/FindCEF.cmake")
        set(CEF_ROOT "$ENV{HOME}/.local/share/explorer-cef")
    elseif(EXISTS "/opt/explorer-cef/cmake/FindCEF.cmake")
        set(CEF_ROOT "/opt/explorer-cef")
    endif()
endif()

option(EXPLORER_NATIVE_ARCH "Enable -march=native CPU-specific codegen" ON)
set(EXPLORER_OPT_FLAGS -O3 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
if(EXPLORER_NATIVE_ARCH)
    list(APPEND EXPLORER_OPT_FLAGS -march=native)
endif()
set(EXPLORER_LINK_FLAGS "")

option(EXPLORER_PGO_GENERATE "Build with Clang PGO instrumentation (-fprofile-generate)" OFF)
set(EXPLORER_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-raw" CACHE PATH "Directory for raw PGO .profraw profiles")
set(EXPLORER_PGO_USE "" CACHE FILEPATH "Path to merged Clang .profdata file for PGO-optimized build")

if(EXPLORER_PGO_GENERATE)
    message(STATUS "Explorer PGO Stage 1: Enabling instrumentation -> ${EXPLORER_PGO_DIR}")
    list(APPEND EXPLORER_OPT_FLAGS "-fprofile-generate=${EXPLORER_PGO_DIR}")
    list(APPEND EXPLORER_LINK_FLAGS "-fprofile-generate=${EXPLORER_PGO_DIR}")
elseif(NOT "${EXPLORER_PGO_USE}" STREQUAL "" AND EXISTS "${EXPLORER_PGO_USE}")
    message(STATUS "Explorer PGO Stage 2: Using profile data -> ${EXPLORER_PGO_USE} (+ ThinLTO)")
    list(APPEND EXPLORER_OPT_FLAGS
        "-fprofile-use=${EXPLORER_PGO_USE}"
        "-Wno-profile-instr-out-of-date"
        "-Wno-profile-instr-unprofiled"
        "-flto=thin"
    )
    list(APPEND EXPLORER_LINK_FLAGS "-fprofile-use=${EXPLORER_PGO_USE}" "-flto=thin")
endif()

# ------------------------------------------------------------------------------
# Module 1: explorer_hal (Hardware Abstraction Layer — zero UI/CEF dependencies)
# ------------------------------------------------------------------------------
add_library(explorer_hal STATIC
    hal/src/gpu_isolator.cpp
    hal/src/vaapi_prober.cpp
    hal/src/power_governor.cpp
    hal/src/display_refresh.cpp
    hal/src/process_throttler.cpp
    hal/src/telemetry.cpp
)
target_include_directories(explorer_hal PUBLIC hal/include)
target_compile_options(explorer_hal PRIVATE ${EXPLORER_OPT_FLAGS})
target_link_options(explorer_hal PUBLIC ${EXPLORER_LINK_FLAGS})
target_link_libraries(explorer_hal PUBLIC PkgConfig::SYSTEMD dl pthread)

# ------------------------------------------------------------------------------
# Module 2: explorer_policy (Pure Deterministic Policy Engine)
# ------------------------------------------------------------------------------
add_library(explorer_policy STATIC
    policy/src/power_policy.cpp
    policy/src/media_policy.cpp
    policy/src/throttle_planner.cpp
    policy/src/navigation_policy.cpp
)
target_include_directories(explorer_policy PUBLIC policy/include)
target_compile_options(explorer_policy PRIVATE ${EXPLORER_OPT_FLAGS})
target_link_options(explorer_policy PUBLIC ${EXPLORER_LINK_FLAGS})
target_link_libraries(explorer_policy PUBLIC explorer_hal)

# ------------------------------------------------------------------------------
# Module 3: explorer_shield (Brave adblock-rust Native Network Blocker + Updater)
# ------------------------------------------------------------------------------
set(BRAVE_SHIELD_LIB "${CMAKE_CURRENT_SOURCE_DIR}/shield-brave/target/release/libexplorer_shield_brave.a")
add_library(explorer_shield STATIC
    shield/src/content_shield.cpp
)
target_include_directories(explorer_shield PUBLIC shield/include)
target_compile_options(explorer_shield PRIVATE ${EXPLORER_OPT_FLAGS})
target_link_options(explorer_shield PUBLIC ${EXPLORER_LINK_FLAGS})
target_link_libraries(explorer_shield PUBLIC "${BRAVE_SHIELD_LIB}" dl pthread m)

# ------------------------------------------------------------------------------
# Module 4: explorer_vault (AES-256-GCM Encrypted Password Vault)
# ------------------------------------------------------------------------------
add_library(explorer_vault STATIC
    vault/src/password_vault.cpp
)
target_include_directories(explorer_vault PUBLIC vault/include)
target_compile_options(explorer_vault PRIVATE ${EXPLORER_OPT_FLAGS})
target_link_options(explorer_vault PUBLIC ${EXPLORER_LINK_FLAGS})
target_link_libraries(explorer_vault PUBLIC PkgConfig::CRYPTO)

# ------------------------------------------------------------------------------
# Module 5: explorer_webapp (Standalone Chromeless Linux WebApp Installer)
# ------------------------------------------------------------------------------
add_library(explorer_webapp STATIC
    webapp/src/webapp_installer.cpp
)
target_include_directories(explorer_webapp PUBLIC webapp/include)
target_compile_options(explorer_webapp PRIVATE ${EXPLORER_OPT_FLAGS})
target_link_options(explorer_webapp PUBLIC ${EXPLORER_LINK_FLAGS})

# ------------------------------------------------------------------------------
# Unit Tests (5 Independent SOLID Test Suites)
# ------------------------------------------------------------------------------
if(BUILD_TESTING)
    add_executable(test_hal hal/tests/test_hal.cpp)
    target_link_libraries(test_hal PRIVATE explorer_hal)
    add_test(NAME explorer_hal_tests COMMAND test_hal)

    add_executable(test_policy policy/tests/test_policy.cpp)
    target_link_libraries(test_policy PRIVATE explorer_policy)
    add_test(NAME explorer_policy_tests COMMAND test_policy)

    add_executable(test_shield shield/tests/test_shield.cpp)
    target_link_libraries(test_shield PRIVATE explorer_shield)
    add_test(NAME explorer_shield_tests COMMAND test_shield)

    add_executable(test_vault vault/tests/test_vault.cpp)
    target_link_libraries(test_vault PRIVATE explorer_vault)
    add_test(NAME explorer_vault_tests COMMAND test_vault)

    add_executable(test_webapp webapp/tests/test_webapp.cpp)
    target_link_libraries(test_webapp PRIVATE explorer_webapp)
    add_test(NAME explorer_webapp_tests COMMAND test_webapp)

    add_executable(test_app_stores app/tests/test_app_stores.cpp app/src/session_store.cpp)
    target_include_directories(test_app_stores PRIVATE app/include)
    target_compile_options(test_app_stores PRIVATE ${EXPLORER_OPT_FLAGS})
    target_link_options(test_app_stores PRIVATE ${EXPLORER_LINK_FLAGS})
    add_test(NAME explorer_app_stores_tests COMMAND test_app_stores)
endif()

# ------------------------------------------------------------------------------
# Module 6 & 7: explorer_engine (CEF 158 Alloy) + explorer Native App
# ------------------------------------------------------------------------------
if(DEFINED CEF_ROOT AND EXISTS "${CEF_ROOT}/cmake/FindCEF.cmake")
    message(STATUS "Explorer: Building Blink/V8 engine against CEF at ${CEF_ROOT}")
    list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
    find_package(CEF REQUIRED)

    add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)

    add_library(explorer_engine STATIC
        engine/src/cef_flags.cpp
        engine/src/cef_client.cpp
    )
    target_include_directories(explorer_engine PUBLIC engine/include "${CEF_ROOT}")
    target_compile_options(explorer_engine PRIVATE ${EXPLORER_OPT_FLAGS})
    target_link_options(explorer_engine PUBLIC ${EXPLORER_LINK_FLAGS})
    target_link_libraries(explorer_engine PUBLIC
        explorer_hal
        explorer_policy
        explorer_shield
        libcef_dll_wrapper
    )

    add_executable(explorer
        app/src/session_store.cpp
        app/src/browser_window.cpp
        app/src/main.cpp
    )
    target_include_directories(explorer PRIVATE app/include "${CEF_ROOT}")
    target_compile_options(explorer PRIVATE ${EXPLORER_OPT_FLAGS})
    target_link_options(explorer PRIVATE ${EXPLORER_LINK_FLAGS})
    target_link_libraries(explorer PRIVATE
        explorer_engine
        explorer_vault
        explorer_webapp
        "${CEF_ROOT}/Release/libcef.so"
        dl
        pthread
    )
    set_target_properties(explorer PROPERTIES
        BUILD_RPATH "${CEF_ROOT}/Release"
        INSTALL_RPATH "$ORIGIN:${CEF_ROOT}/Release"
    )
else()
    message(WARNING "CEF_ROOT not found — built HAL/Policy/Shield/Vault/WebApp + unit tests only.")
endif()

set(deps_cc "${CMAKE_C_COMPILER}")
set(cross_host "")
set(cross_rc "")
if(CMAKE_CROSSCOMPILING)
    if(APPLE_TARGET_TRIPLE)
        if(PLATFORM MATCHES "OS64" OR PLATFORM MATCHES "SIMULATORARM64")
            set(APPLE_TARGET_TRIPLE aarch64-apple-ios)
        elseif(PLATFORM MATCHES "SIMULATOR64")
            set(APPLE_TARGET_TRIPLE x86_64-apple-ios)
        endif()
        set(cross_host "--host=${APPLE_TARGET_TRIPLE}")
    elseif(ANDROID)
        if(CMAKE_ANDROID_ARCH_ABI MATCHES x86_64)
            set(cross_host "--host=x86_64-linux-android")
            set(android_compiler_prefix x86_64)
            set(android_compiler_suffix linux-android)
        elseif(CMAKE_ANDROID_ARCH_ABI MATCHES x86)
            set(cross_host "--host=i686-linux-android")
            set(android_compiler_prefix i686)
            set(android_compiler_suffix linux-android)
        elseif(CMAKE_ANDROID_ARCH_ABI MATCHES armeabi-v7a)
            set(cross_host "--host=armv7a-linux-androideabi")
            set(android_compiler_prefix armv7a)
            set(android_compiler_suffix linux-androideabi)
        elseif(CMAKE_ANDROID_ARCH_ABI MATCHES arm64-v8a)
            set(cross_host "--host=aarch64-linux-android")
            set(android_compiler_prefix aarch64)
            set(android_compiler_suffix linux-android)
        else()
            message(FATAL_ERROR "unknown android arch: ${CMAKE_ANDROID_ARCH_ABI}")
        endif()

        string(REPLACE "android-" "" android_platform_num "${ANDROID_PLATFORM}")
        set(deps_cc "${ANDROID_TOOLCHAIN_ROOT}/bin/${android_compiler_prefix}-${android_compiler_suffix}${android_platform_num}-clang")
    else()
        set(cross_host "--host=${ARCH_TRIPLET}")
        if (ARCH_TRIPLET MATCHES mingw AND CMAKE_RC_COMPILER)
            set(cross_rc "WINDRES=${CMAKE_RC_COMPILER}")
        endif()
    endif()
endif()


add_subdirectory(oxen-encoding)


if(CMAKE_C_COMPILER_LAUNCHER)
  set(deps_cc "${CMAKE_C_COMPILER_LAUNCHER} ${deps_cc}")
endif()
set(deps_CFLAGS "-O2")

if(IPO_ENABLED)
  set(deps_CFLAGS "${deps_CFLAGS} -flto")
endif()

if(APPLE)
    foreach(lang C CXX)
        string(APPEND deps_${lang}FLAGS " ${CMAKE_${lang}_SYSROOT_FLAG} ${CMAKE_OSX_SYSROOT} ${CMAKE_${lang}_OSX_DEPLOYMENT_TARGET_FLAG}${CMAKE_OSX_DEPLOYMENT_TARGET}")
        foreach(arch ${CMAKE_OSX_ARCHITECTURES})
            string(APPEND deps_${lang}FLAGS " -arch ${arch}")
        endforeach()
    endforeach()
endif()


add_subdirectory(libsodium-internal)


set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "")
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "")
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "")
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "")
set(ZSTD_BUILD_STATIC ON CACHE BOOL "")
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "")
add_subdirectory(zstd/build/cmake)
# zstd's cmake doesn't properly set up include paths on its targets, so we have to wrap it in an
# interface target that does:
add_library(libzstd_static_fixed_includes INTERFACE)
target_include_directories(libzstd_static_fixed_includes INTERFACE zstd/lib zstd/lib/common)
target_link_libraries(libzstd_static_fixed_includes INTERFACE libzstd_static)
add_library(libzstd::static ALIAS libzstd_static_fixed_includes)
export(
    TARGETS libzstd_static_fixed_includes
    NAMESPACE libsession::
    FILE libsessionZstd.cmake
)

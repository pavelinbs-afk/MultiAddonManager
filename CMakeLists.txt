cmake_minimum_required(VERSION 3.16)

# multiaddonmanager — MetaMod:Source 2 plugin (CS2)
# Output: build/addons/multiaddonmanager/bin/linuxsteamrt64/multiaddonmanager.so

project(multiaddonmanager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MAM_OPTIMIZE "Enable -O3 in Release" ON)

function(mam_try_path OUT_VAR)
  set(${OUT_VAR} "" PARENT_SCOPE)
  foreach(candidate IN LISTS ARGN)
    if(candidate AND EXISTS "${candidate}")
      set(${OUT_VAR} "${candidate}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
endfunction()

if(NOT HL2SDK_PATH AND DEFINED ENV{HL2SDKCS2})
  set(HL2SDK_PATH "$ENV{HL2SDKCS2}")
endif()
if(NOT MMS_PATH AND DEFINED ENV{MMSOURCE112})
  set(MMS_PATH "$ENV{MMSOURCE112}")
elseif(NOT MMS_PATH AND DEFINED ENV{MMSOURCE_DEV})
  set(MMS_PATH "$ENV{MMSOURCE_DEV}")
endif()

set(_mam_roots
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/.."
  "${CMAKE_SOURCE_DIR}/../.."
)
foreach(_root IN LISTS _mam_roots)
  list(APPEND _mam_hl2sdk_candidates "${_root}/deps/hl2sdk-cs2")
  list(APPEND _mam_mms_candidates "${_root}/deps/metamod-source")
endforeach()

if(NOT HL2SDK_PATH OR NOT EXISTS "${HL2SDK_PATH}/public/tier0/dbg.h")
  mam_try_path(_found_hl2sdk ${_mam_hl2sdk_candidates})
  if(_found_hl2sdk)
    set(HL2SDK_PATH "${_found_hl2sdk}")
  endif()
endif()
if(NOT MMS_PATH OR NOT EXISTS "${MMS_PATH}/core/ISmmPlugin.h")
  mam_try_path(_found_mms ${_mam_mms_candidates})
  if(_found_mms)
    set(MMS_PATH "${_found_mms}")
  endif()
endif()

set(HL2SDK_PATH "${HL2SDK_PATH}" CACHE PATH "Path to hl2sdk-cs2")
set(MMS_PATH "${MMS_PATH}" CACHE PATH "Path to metamod-source")

if(NOT HL2SDK_PATH OR NOT EXISTS "${HL2SDK_PATH}/public/tier0/dbg.h")
  message(FATAL_ERROR "HL2SDK not found. Run: bash build_cmake.sh")
endif()
if(NOT MMS_PATH OR NOT EXISTS "${MMS_PATH}/core/ISmmPlugin.h")
  message(FATAL_ERROR "Metamod:Source not found. Run: bash build_cmake.sh")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(MMS_CORE "${MMS_PATH}/core")

execute_process(
  COMMAND git describe --tags --long
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE MAM_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT MAM_VERSION)
  set(MAM_VERSION "1.3-dev")
endif()

add_library(mam_flags INTERFACE)

target_compile_definitions(mam_flags INTERFACE
  META_IS_SOURCE2
  SOURCE_ENGINE=25
  SE_CS2=25
  GAME_DLL
  RAD_TELEMETRY_DISABLED
  stricmp=strcasecmp
  _stricmp=strcasecmp
  _snprintf=snprintf
  _vsnprintf=vsnprintf
  HAVE_STDINT_H
  GNUC
  "MULTIADDONMANAGER_VERSION=\"${MAM_VERSION}\""
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(mam_flags INTERFACE
    -pipe
    -fno-strict-aliasing
    -Wall
    -Wno-uninitialized
    -Wno-parentheses
    -Wno-unused
    -Wno-switch
    -Wno-sign-compare
    -Wno-attributes
    -Wno-invalid-offsetof
    -Wno-non-virtual-dtor
    -Wno-overloaded-virtual
    -Wno-register
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -fno-omit-frame-pointer
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>
  )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(mam_flags INTERFACE
    LINUX
    _LINUX
    POSIX
    _FILE_OFFSET_BITS=64
    X64BITS
    PLATFORM_64BITS
    COMPILER_GCC
    _GLIBCXX_USE_CXX11_ABI=0
  )
  target_compile_options(mam_flags INTERFACE -fPIC -msse)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" AND MAM_OPTIMIZE)
    target_compile_options(mam_flags INTERFACE -O3)
    target_compile_definitions(mam_flags INTERFACE NDEBUG)
  endif()
endif()

target_include_directories(mam_flags INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/utils"
  "${CMAKE_CURRENT_SOURCE_DIR}/public"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/funchook/include"
  "${MMS_CORE}"
  "${MMS_PATH}/third_party/khook/include"
  "${HL2SDK_PATH}/thirdparty/protobuf-3.21.8/src"
  "${HL2SDK_PATH}/public"
  "${HL2SDK_PATH}/public/engine"
  "${HL2SDK_PATH}/public/mathlib"
  "${HL2SDK_PATH}/public/tier0"
  "${HL2SDK_PATH}/public/tier1"
  "${HL2SDK_PATH}/public/entity2"
  "${HL2SDK_PATH}/public/game/server"
  "${HL2SDK_PATH}/public/schemasystem"
  "${HL2SDK_PATH}/public/steam"
  "${HL2SDK_PATH}/game/shared"
  "${HL2SDK_PATH}/game/server"
  "${HL2SDK_PATH}/common"
)

set(MAM_PROTO_FILES
  "${HL2SDK_PATH}/common/valveextensions.proto"
  "${HL2SDK_PATH}/common/network_connection.proto"
  "${HL2SDK_PATH}/common/networkbasetypes.proto"
  "${HL2SDK_PATH}/game/shared/gameevents.proto"
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(MAM_PROTOC "${HL2SDK_PATH}/devtools/bin/linux/protoc")
else()
  set(MAM_PROTOC "${HL2SDK_PATH}/devtools/bin/protoc.exe")
endif()

if(NOT EXISTS "${MAM_PROTOC}")
  message(FATAL_ERROR "protoc not found: ${MAM_PROTOC}")
endif()

set(MAM_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MAM_PROTO_GEN_DIR}")

set(MAM_PROTO_SRCS "")
set(MAM_PROTO_HDRS "")

foreach(proto ${MAM_PROTO_FILES})
  get_filename_component(proto_name "${proto}" NAME_WE)
  set(out_cc "${MAM_PROTO_GEN_DIR}/${proto_name}.pb.cc")
  set(out_h "${MAM_PROTO_GEN_DIR}/${proto_name}.pb.h")
  list(APPEND MAM_PROTO_SRCS "${out_cc}")
  list(APPEND MAM_PROTO_HDRS "${out_h}")

  add_custom_command(
    OUTPUT "${out_cc}" "${out_h}"
    COMMAND "${MAM_PROTOC}"
      --cpp_out=${MAM_PROTO_GEN_DIR}
      "-I${HL2SDK_PATH}/common"
      "-I${HL2SDK_PATH}/game/shared"
      "-I${HL2SDK_PATH}/thirdparty/protobuf-3.21.8/src"
      "${proto}"
    DEPENDS "${proto}"
    COMMENT "Generating ${proto_name}.pb.cc"
    VERBATIM
  )
endforeach()

add_custom_target(mam_protoc DEPENDS ${MAM_PROTO_SRCS} ${MAM_PROTO_HDRS})

set(MAM_PLUGIN_SRCS
  src/multiaddonmanager.cpp
  src/tier0_stubs.cpp
  src/utils/plat_unix.cpp
)

set(MAM_SDK_SRCS
  "${HL2SDK_PATH}/public/tier0/memoverride.cpp"
  "${HL2SDK_PATH}/tier1/convar.cpp"
)

add_library(multiaddonmanager SHARED
  ${MAM_PLUGIN_SRCS}
  ${MAM_SDK_SRCS}
  ${MAM_PROTO_SRCS}
)

add_dependencies(multiaddonmanager mam_protoc)

target_link_libraries(multiaddonmanager PRIVATE mam_flags)
target_include_directories(multiaddonmanager PRIVATE "${MAM_PROTO_GEN_DIR}")

set_target_properties(multiaddonmanager PROPERTIES
  PREFIX ""
  OUTPUT_NAME "multiaddonmanager"
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(HL2SDK_LIB "${HL2SDK_PATH}/lib/linux64")
  set(MAM_FUNCHOOK_LIB_DIR "${CMAKE_SOURCE_DIR}/vendor/funchook/lib/Release")
  set(MAM_SDK_LIBS
    "${HL2SDK_LIB}/mathlib.a"
    "${HL2SDK_LIB}/interfaces.a"
    "${HL2SDK_LIB}/release/libprotobuf.a"
    "${MAM_FUNCHOOK_LIB_DIR}/libfunchook.a"
    "${MAM_FUNCHOOK_LIB_DIR}/libdistorm.a"
  )
  foreach(lib ${MAM_SDK_LIBS})
    if(NOT EXISTS "${lib}")
      message(FATAL_ERROR "Missing library: ${lib}")
    endif()
  endforeach()

  target_link_libraries(multiaddonmanager PRIVATE
    ${MAM_SDK_LIBS}
    dl
    pthread
    m
  )

  target_link_options(multiaddonmanager PRIVATE
    -static-libgcc
    -static-libstdc++
  )

  set(MAM_OUT_DIR "${CMAKE_BINARY_DIR}/addons/multiaddonmanager/bin/linuxsteamrt64")
  set_target_properties(multiaddonmanager PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${MAM_OUT_DIR}"
  )

  set(MAM_PKG_METAMOD "${CMAKE_BINARY_DIR}/addons/metamod")
  set(MAM_PKG_CFG "${CMAKE_BINARY_DIR}/cfg/multiaddonmanager")
  set(MAM_VDF "${CMAKE_BINARY_DIR}/multiaddonmanager.vdf")
  file(WRITE "${MAM_VDF}" "\"Metamod Plugin\"\n{\n\t\"alias\"\t\"multiaddonmanager\"\n\t\"file\"\t\"addons/multiaddonmanager/bin/multiaddonmanager\"\n}\n")

  add_custom_target(mam_package ALL
    DEPENDS multiaddonmanager
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MAM_PKG_METAMOD}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MAM_PKG_CFG}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${MAM_VDF}" "${MAM_PKG_METAMOD}/multiaddonmanager.vdf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/cfg/multiaddonmanager/multiaddonmanager.cfg" "${MAM_PKG_CFG}/"
    COMMENT "Packaging multiaddonmanager layout"
  )
else()
  message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME} (use WSL/Linux build_cmake.sh)")
endif()

message(STATUS "multiaddonmanager: version=${MAM_VERSION}")
message(STATUS "multiaddonmanager: HL2SDK=${HL2SDK_PATH}")
message(STATUS "multiaddonmanager: MMS=${MMS_PATH}")

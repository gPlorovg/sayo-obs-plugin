cmake_minimum_required(VERSION 3.28...3.30)

project(sayo-obs-plugin VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32 AND MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  add_compile_definitions(_USE_STD_VECTOR_ALGORITHMS=0)
endif()

find_package(Protobuf REQUIRED)

find_package(gRPC CONFIG QUIET)
if(gRPC_FOUND)
  set(SAYO_GRPC_LIBRARIES gRPC::grpc++ gRPC::grpc)
  set(SAYO_GRPC_CPP_PLUGIN "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")
else()
  find_path(SAYO_GRPC_INCLUDE_DIR grpcpp/grpcpp.h REQUIRED)
  find_library(SAYO_GRPCXX_LIBRARY NAMES grpc++ REQUIRED)
  find_library(SAYO_GRPC_LIBRARY NAMES grpc REQUIRED)
  find_program(SAYO_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
  set(SAYO_GRPC_LIBRARIES "${SAYO_GRPCXX_LIBRARY}" "${SAYO_GRPC_LIBRARY}")
endif()

find_package(SampleRate CONFIG QUIET)
if(NOT SampleRate_FOUND)
  find_path(SAYO_SAMPLERATE_INCLUDE_DIR samplerate.h REQUIRED)
  find_library(SAYO_SAMPLERATE_LIBRARY NAMES samplerate REQUIRED)

  add_library(SampleRate::samplerate UNKNOWN IMPORTED)
  set_target_properties(SampleRate::samplerate PROPERTIES
    IMPORTED_LOCATION "${SAYO_SAMPLERATE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SAYO_SAMPLERATE_INCLUDE_DIR}"
  )
endif()

find_package(libobs QUIET)
if(NOT libobs_FOUND AND NOT TARGET OBS::libobs)
  find_path(SAYO_LIBOBS_INCLUDE_DIR obs-module.h
    PATH_SUFFIXES obs
    REQUIRED
  )
  find_library(SAYO_LIBOBS_LIBRARY NAMES obs libobs REQUIRED)

  add_library(OBS::libobs UNKNOWN IMPORTED)
  set_target_properties(OBS::libobs PROPERTIES
    IMPORTED_LOCATION "${SAYO_LIBOBS_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SAYO_LIBOBS_INCLUDE_DIR}"
  )
endif()

set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/proto/sayo.proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

set(PROTO_SRCS "${GENERATED_DIR}/sayo.pb.cc" "${GENERATED_DIR}/sayo.grpc.pb.cc")
set(PROTO_HDRS "${GENERATED_DIR}/sayo.pb.h" "${GENERATED_DIR}/sayo.grpc.pb.h")

add_custom_command(
  OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
  COMMAND protobuf::protoc
  ARGS
    --proto_path "${CMAKE_CURRENT_SOURCE_DIR}/proto"
    --cpp_out "${GENERATED_DIR}"
    --grpc_out "${GENERATED_DIR}"
    --plugin=protoc-gen-grpc=${SAYO_GRPC_CPP_PLUGIN}
    "${PROTO_FILE}"
  DEPENDS "${PROTO_FILE}"
  COMMENT "Generating C++ protobuf/grpc stubs from sayo.proto"
  VERBATIM
)

add_library(sayo_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})
target_include_directories(sayo_proto PUBLIC "${GENERATED_DIR}")
if(SAYO_GRPC_INCLUDE_DIR)
  target_include_directories(sayo_proto PUBLIC "${SAYO_GRPC_INCLUDE_DIR}")
endif()
target_link_libraries(sayo_proto PUBLIC protobuf::libprotobuf ${SAYO_GRPC_LIBRARIES})

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin-support.c.in"
  "${CMAKE_CURRENT_BINARY_DIR}/plugin-support.c"
  @ONLY
)

set(PLUGIN_SOURCES
  src/plugin-main.cpp
  src/grpc_transport.cpp
  src/subtitle_buffer.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/plugin-support.c"
)

add_library(sayo_obs_plugin MODULE ${PLUGIN_SOURCES})

target_include_directories(sayo_obs_plugin
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GENERATED_DIR}"
)

target_link_libraries(sayo_obs_plugin
  PRIVATE
    sayo_proto
    SampleRate::samplerate
    OBS::libobs
)

if(WIN32 OR (UNIX AND NOT APPLE))
  set_target_properties(sayo_obs_plugin PROPERTIES PREFIX "")
endif()

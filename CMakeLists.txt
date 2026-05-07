cmake_minimum_required(VERSION 3.28...3.30)

project(sayo-obs-plugin VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32 AND MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  add_compile_definitions(_USE_STD_VECTOR_ALGORITHMS=0)
endif()

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(SampleRate CONFIG REQUIRED)
find_package(libobs REQUIRED)

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
    --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
    "${PROTO_FILE}"
  DEPENDS "${PROTO_FILE}"
  COMMENT "Generating C++ protobuf/grpc stubs from sayo.proto"
  VERBATIM
)

add_library(sayo_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})
target_include_directories(sayo_proto PUBLIC "${GENERATED_DIR}")
target_link_libraries(sayo_proto PUBLIC protobuf::libprotobuf gRPC::grpc++ gRPC::grpc)

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

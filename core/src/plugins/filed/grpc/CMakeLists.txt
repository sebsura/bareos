#   BAREOS® - Backup Archiving REcovery Open Sourced
#
#   Copyright (C) 2024-2024 Bareos GmbH & Co. KG
#
#   This program is Free Software; you can redistribute it and/or
#   modify it under the terms of version three of the GNU Affero General Public
#   License as published by the Free Software Foundation and included
#   in the file LICENSE.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#   Affero General Public License for more details.
#
#   You should have received a copy of the GNU Affero General Public License
#   along with this program; if not, write to the Free Software
#   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
#   02110-1301, USA.

include(BareosDisableWarnings)
include(BareosConfigureFile)

if(HAVE_WIN32)
  message(STATUS "Disabling grpc plugin as its not supported on windows")
elseif(HAVE_GRPC)
  message(STATUS "Using protobuf ${Protobuf_VERSION}")
  message(STATUS "Using gRPC ${gRPC_VERSION}")

  set(_PROTOBUF_LIBPROTOBUF protobuf::libprotobuf)
  if(TARGET gRPC::grpc++_reflection)
    set(_REFLECTION gRPC::grpc++_reflection)
  else()
    set(_REFLECTION grpc++_reflection)
  endif()
  if(CMAKE_CROSSCOMPILING)
    find_program(_PROTOBUF_PROTOC protoc)
  else()
    set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
  endif()
  if(TARGET gRPC::grpc++)
    set(_GRPC_GRPCPP gRPC::grpc++)
  else()
    set(_GRPC_GRPCPP grpc++)
  endif()
  if(TARGET gRPC::grpc_cpp_plugin)
    set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
  else()
    find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
  endif()

  set(PROTO_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
  # Proto file
  function(target_add_proto_sources target)
    set(protosrcs)
    set(protohdrs)
    set(grpcsrcs)
    set(grpchdrs)

    foreach(file ${ARGN})
      get_filename_component(absolute "${file}" ABSOLUTE)
      get_filename_component(path "${absolute}" PATH)
      get_filename_component(stem "${file}" NAME_WE)

      set(proto_src "${PROTO_OUTPUT_DIR}/${stem}.pb.cc")
      set(proto_hdr "${PROTO_OUTPUT_DIR}/${stem}.pb.h")
      set(grpc_src "${PROTO_OUTPUT_DIR}/${stem}.grpc.pb.cc")
      set(grpc_hdr "${PROTO_OUTPUT_DIR}/${stem}.grpc.pb.h")
        if(Protobuf_VERSION VERSION_LESS 3.15.0)
          set(_PROTOBUF_PROTOC_EXTRA_PARAMS "--experimental_allow_proto3_optional")
        else()
          set(_PROTOBUF_PROTOC_EXTRA_PARAMS "")
        endif()
      add_custom_command(
        OUTPUT "${proto_src}" "${proto_hdr}" "${grpc_src}" "${grpc_hdr}"
        COMMAND
          ${_PROTOBUF_PROTOC} ARGS --grpc_out "${PROTO_OUTPUT_DIR}"
          --cpp_out "${PROTO_OUTPUT_DIR}" -I "${path}"
          --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
          ${_PROTOBUF_PROTOC_EXTRA_PARAMS}
          "${absolute}"
        DEPENDS "${absolute}"
      )

      list(APPEND protosrcs "${proto_src}")
      list(APPEND protohdrs "${proto_hdr}")
      list(APPEND grpcsrcs "${grpc_src}")
      list(APPEND grpchdrs "${grpc_hdr}")
    endforeach()

    target_sources(${target} PRIVATE ${protosrcs} ${grpcsrcs})
    target_include_directories(${target} PUBLIC ${PROTO_OUTPUT_DIR})
  endfunction()

  add_library(grpc-proto OBJECT)
  target_add_proto_sources(grpc-proto
    "proto/plugin.proto" "proto/events.proto" "proto/bareos.proto"
    "proto/common.proto"
  )
  target_link_libraries(
    grpc-proto PUBLIC ${_REFLECTION} ${_GRPC_GRPCPP} ${_PROTOBUF_LIBPROTOBUF}
  )

  add_library(grpc-fd MODULE grpc.cc grpc_impl.cc bareos_api.cc)
  set_target_properties(
    grpc-fd PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ..
  )
  target_link_libraries(grpc-fd bareos fmt grpc-proto)
  install(
    TARGETS grpc-fd
    DESTINATION ${plugindir}
    COMPONENT filedaemon
  )

  add_executable(grpc-test-module grpc_test_module.cc plugin_service.cc)
  target_link_libraries(grpc-test-module fmt grpc-proto)
  install(
    TARGETS grpc-test-module
    DESTINATION ${plugindir}
    COMPONENT filedaemon
  )

  add_executable(
    grpc-python-module grpc_python_module.cc plugin_service_python.cc
  )
  target_link_libraries(grpc-python-module fmt grpc-proto ${Python3_LIBRARIES})
  target_include_directories(grpc-python-module PUBLIC ${Python3_INCLUDE_DIRS})
  install(
    TARGETS grpc-python-module
    DESTINATION ${plugindir}
    COMPONENT filedaemon
  )

  set_target_properties(
    grpc-fd PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ..
  )

  include(DebugEdit)
endif()

cmake_minimum_required(VERSION 3.20)

if(DEFINED ZEPHYR_BASE OR DEFINED CONFIG_RIGLINK)
  # Built as a Zephyr module.
  if(CONFIG_RIGLINK)
    zephyr_library()
    zephyr_library_sources(
      src/riglink_proto.c
      src/riglink_io.c
      src/riglink_cmd.c
      src/riglink_core.c
    )
    zephyr_library_sources_ifdef(CONFIG_RIGLINK_BACKEND_SHELL
      zephyr/shell_backend.c
    )
    zephyr_include_directories(include)
  endif()
else()
  # Standalone static library (non-Zephyr MCU projects / host tools).
  project(riglink VERSION 0.1.0 LANGUAGES C)
  add_subdirectory(third_party/jcon)
  add_library(riglink STATIC
    src/riglink_proto.c
    src/riglink_io.c
    src/riglink_cmd.c
    src/riglink_core.c)
  target_include_directories(riglink PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(riglink PUBLIC jcon::jcon)
  target_compile_features(riglink PUBLIC c_std_11)
endif()

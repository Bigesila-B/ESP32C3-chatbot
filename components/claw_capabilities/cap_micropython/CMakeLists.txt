idf_component_register(
    SRCS "cap_micropython.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES claw_cap claw_core claw_utils json esp_timer esp_hw_support
)

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
# MicroPython v1.28.0. Set FETCHCONTENT_SOURCE_DIR_CLAW_MICROPYTHON to reuse a checkout.
FetchContent_Declare(claw_micropython
    URL "https://codeload.github.com/micropython/micropython/tar.gz/e0e9fbb17ed6fd06bb76e266ae554784c9c80804"
    URL_HASH SHA256=8065be41eab48b889ca52966743f2217c9c210e109fd18661ea02b97c6cc618c
)
FetchContent_MakeAvailable(claw_micropython)

set(MICROPY_DIR "${claw_micropython_SOURCE_DIR}")
include("${MICROPY_DIR}/py/py.cmake")
set(micropython_build_dir "${CMAKE_CURRENT_BINARY_DIR}")
set(micropython_sources ${MICROPY_SOURCE_PY} "${MICROPY_DIR}/extmod/modjson.c")
set(micropython_headers
    "${micropython_build_dir}/genhdr/mpversion.h"
    "${micropython_build_dir}/genhdr/qstrdefs.generated.h"
    "${micropython_build_dir}/genhdr/moduledefs.h"
    "${micropython_build_dir}/genhdr/root_pointers.h"
)
idf_build_get_property(idf_python PYTHON)
file(GLOB_RECURSE micropython_source_headers CONFIGURE_DEPENDS
    "${MICROPY_DIR}/py/*.h" "${MICROPY_DIR}/extmod/*.h")
add_custom_command(
    OUTPUT ${micropython_headers}
    COMMAND "${idf_python}" "${CMAKE_CURRENT_LIST_DIR}/generate_headers.py"
        "${CMAKE_C_COMPILER}" "${MICROPY_DIR}" "${micropython_build_dir}"
        "${CMAKE_CURRENT_LIST_DIR}" ${micropython_sources}
    DEPENDS ${micropython_sources} ${micropython_source_headers}
        "${CMAKE_CURRENT_LIST_DIR}/generate_headers.py" "${CMAKE_CURRENT_LIST_DIR}/mpconfigport.h"
        "${MICROPY_DIR}/ports/embed/port/mpconfigport_common.h"
        "${MICROPY_DIR}/ports/embed/port/mphalport.h"
        "${MICROPY_DIR}/py/makeqstrdefs.py" "${MICROPY_DIR}/py/makeqstrdata.py"
        "${MICROPY_DIR}/py/makemoduledefs.py" "${MICROPY_DIR}/py/make_root_pointers.py"
        "${MICROPY_DIR}/py/makeversionhdr.py" "${MICROPY_DIR}/py/mpconfig.h"
    VERBATIM
)
add_custom_target(cap_micropython_headers DEPENDS ${micropython_headers})
add_dependencies(${COMPONENT_LIB} cap_micropython_headers)

set(micropython_gc_source "${MICROPY_DIR}/ports/esp32/gccollect.c")
# The ESP32 port normally gets this declaration through its own port config.
set_source_files_properties("${micropython_gc_source}" PROPERTIES COMPILE_OPTIONS "-include;esp_cpu.h")
target_sources(${COMPONENT_LIB} PRIVATE ${micropython_sources} "${micropython_gc_source}")
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}" "${MICROPY_DIR}" "${MICROPY_DIR}/ports/embed" "${micropython_build_dir}")

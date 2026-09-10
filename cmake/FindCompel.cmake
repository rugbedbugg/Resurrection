# FindCompel — locate CRIU's compel, used for the Phase 2 restorer bootstrap.
#
# Two distinct sets of outputs, because compel serves two very different roles:
#
#   Host side  — arctl includes <compel/infect.h> and links libcompel to drive
#                injection from outside the target.
#   Blob side  — the injected parasite is built freestanding with the compiler
#                flags compel itself dictates (no libc, no stack protector, PIE),
#                linked with its own linker script, against its `std` plugin.
#
# Mixing the two silently produces a blob that cannot run, so they are kept apart.
#
# Defines:
#   Compel_FOUND
#   COMPEL_EXECUTABLE      the `compel` driver binary
#   COMPEL_INCLUDE_DIR     directory containing compel/infect.h
#   COMPEL_LIBRARY         libcompel for the host program
#   COMPEL_VERSION
#   COMPEL_PARASITE_CFLAGS   ) flags for building the injected blob,
#   COMPEL_PARASITE_LDFLAGS  ) as reported by compel itself -- never
#   COMPEL_PARASITE_PLUGINS  ) hardcode these, they are toolchain specific

find_program(COMPEL_EXECUTABLE NAMES compel)
find_path(COMPEL_INCLUDE_DIR NAMES compel/infect.h)
find_library(COMPEL_LIBRARY NAMES compel)

# Ask compel for the blob-side toolchain flags. `compel includes` is empty on a
# distro install (headers sit on the default search path) and non-empty when
# compel lives somewhere unusual, so it is appended rather than relied upon.
function(_compel_query var)
    execute_process(COMMAND ${COMPEL_EXECUTABLE} ${ARGN}
                    OUTPUT_VARIABLE _out
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET
                    RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        set(${var} "" PARENT_SCOPE)
    else()
        separate_arguments(_out NATIVE_COMMAND "${_out}")
        set(${var} "${_out}" PARENT_SCOPE)
    endif()
endfunction()

if(COMPEL_EXECUTABLE)
    _compel_query(COMPEL_PARASITE_CFLAGS cflags)
    _compel_query(COMPEL_PARASITE_LDFLAGS ldflags)
    _compel_query(COMPEL_PARASITE_PLUGINS plugins std)
    _compel_query(_compel_extra_includes includes)
    list(APPEND COMPEL_PARASITE_CFLAGS ${_compel_extra_includes})

    execute_process(COMMAND ${COMPEL_EXECUTABLE} --version
                    OUTPUT_VARIABLE _ver OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" COMPEL_VERSION "${_ver}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Compel
    REQUIRED_VARS COMPEL_EXECUTABLE COMPEL_LIBRARY COMPEL_INCLUDE_DIR
    VERSION_VAR COMPEL_VERSION)

if(Compel_FOUND AND NOT TARGET Compel::Compel)
    add_library(Compel::Compel UNKNOWN IMPORTED)
    set_target_properties(Compel::Compel PROPERTIES
        IMPORTED_LOCATION "${COMPEL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${COMPEL_INCLUDE_DIR}")
endif()

mark_as_advanced(COMPEL_EXECUTABLE COMPEL_INCLUDE_DIR COMPEL_LIBRARY)

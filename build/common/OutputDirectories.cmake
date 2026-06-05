# ==================================================================================================
# Unified build output layout (Windows / multi-config generators)
#
#   <build-dir>/intermediate/   MSVC .obj, .pch, per-target build scratch
#   <build-dir>/bin/<Config>/   .exe, .dll (runnable together)
#   <build-dir>/lib/<Config>/   .lib static libraries
#   <build-dir>/bin/assets/     sample runtime assets (when samples are built)
# ==================================================================================================

option(FILAMENT_SEPARATE_OUTPUT_DIRS
    "Place final binaries in bin/, libraries in lib/, intermediates in intermediate/"
    ON)

macro(filament_setup_output_directories)
    if (FILAMENT_SEPARATE_OUTPUT_DIRS)

    set(FILAMENT_OUTPUT_BIN "${CMAKE_BINARY_DIR}/bin" CACHE PATH
        "Directory for executables and shared libraries")
    set(FILAMENT_OUTPUT_LIB "${CMAKE_BINARY_DIR}/lib" CACHE PATH
        "Directory for static libraries (.lib)")
    set(FILAMENT_OUTPUT_INT "${CMAKE_BINARY_DIR}/intermediate" CACHE PATH
        "Directory for intermediate build files (.obj, .pch, etc.)")

    get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

    if (_is_multi_config)
        foreach(_config ${CMAKE_CONFIGURATION_TYPES})
            string(TOUPPER ${_config} _config_upper)
            set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_config_upper} "${FILAMENT_OUTPUT_BIN}/${_config}")
            set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_config_upper} "${FILAMENT_OUTPUT_LIB}/${_config}")
            # .dll next to .exe so a config folder is self-contained.
            if (WIN32)
                set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_config_upper} "${FILAMENT_OUTPUT_BIN}/${_config}")
            else()
                set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_config_upper} "${FILAMENT_OUTPUT_LIB}/${_config}")
            endif()
        endforeach()
    else()
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${FILAMENT_OUTPUT_BIN}")
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${FILAMENT_OUTPUT_BIN}")
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${FILAMENT_OUTPUT_LIB}")
    endif()

    if (MSVC)
        # Redirect MSVC intermediate directory (.obj, .pch) away from scattered *.dir folders.
        file(TO_CMAKE_PATH "${FILAMENT_OUTPUT_INT}" _int_dir)
        set(CMAKE_VS_GLOBALS
            "IntDir=${_int_dir}/$(Configuration)/$(ProjectName)\\"
        )
    endif()

    message(STATUS "Filament output: bin=${FILAMENT_OUTPUT_BIN}, lib=${FILAMENT_OUTPUT_LIB}, intermediate=${FILAMENT_OUTPUT_INT}")
    endif()
endmacro()

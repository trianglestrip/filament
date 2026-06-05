cmake_minimum_required(VERSION 3.19)
project(pbrtio)

set(TARGET pbrtio)
set(PUBLIC_HDR_DIR include)

set(PUBLIC_HDRS
    include/pbrtio/PbrtCompat.h
    include/pbrtio/PbrtFormat.h
    include/pbrtio/PbrtMath.h
    include/pbrtio/PbrtSpectrum.h
    include/pbrtio/PbrtTypes.h
    include/pbrtio/Helpers.h
    include/pbrtio/Parser.h
    include/pbrtio/Parameters.h
    include/pbrtio/Builder.h
    include/pbrtio/LoopSubdivide.h
    include/pbrtio/FilamentPbrtLoader.h
)

set(SRCS
    src/Parser.cpp
    src/Parameters.cpp
    src/Builder.cpp
    src/LoopSubdivide.cpp
    src/FilamentPbrtLoader.cpp
)

include_directories(${PUBLIC_HDR_DIR})
add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${SRCS})
target_include_directories(${TARGET} PUBLIC ${PUBLIC_HDR_DIR})
target_link_libraries(${TARGET} PUBLIC math)
set_target_properties(${TARGET} PROPERTIES FOLDER Libs)

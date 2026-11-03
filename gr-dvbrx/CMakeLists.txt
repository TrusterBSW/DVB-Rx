cmake_minimum_required(VERSION 3.16)
project(gr-dvbrx C CXX)

set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native -ffp-contract=fast -fno-math-errno")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")

find_package(Gnuradio "3.10" REQUIRED COMPONENTS runtime)
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
find_package(pybind11 REQUIRED)

set(LIBDVBRX ${CMAKE_CURRENT_SOURCE_DIR}/../libdvbrx)

# The libdvbrx sources are compiled in so the module has no runtime dependency.
pybind11_add_module(_gr bindings.cc frontend_cc.cc ${LIBDVBRX}/src/frontend.c)
target_include_directories(_gr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${LIBDVBRX}/include)
set_source_files_properties(${LIBDVBRX}/src/frontend.c PROPERTIES COMPILE_OPTIONS "-std=gnu11")
target_link_libraries(_gr PRIVATE gnuradio::gnuradio-runtime m)

# Built straight into the dvbrx Python package.
set_target_properties(_gr PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../python/dvbrx)

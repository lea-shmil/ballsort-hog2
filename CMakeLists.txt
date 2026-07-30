cmake_minimum_required(VERSION 3.16)
project(ballsort-hog2 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release by default: this is a search-timing study, never benchmark a debug build.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# rscbt-par threads its layer expansion with std::thread.
find_package(Threads REQUIRED)

set(HOG2_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/hog2)

# Exactly the five headless include dirs -- no gui/, no SFML, no OpenGL.
set(HOG2_INCLUDE_DIRS
  ${HOG2_ROOT}/search
  ${HOG2_ROOT}/generic
  ${HOG2_ROOT}/algorithms
  ${HOG2_ROOT}/utils
  ${HOG2_ROOT}/simulation
)

# HOG2 headers pull in a few .cpp files from utils/ for symbols they reference
# (colors, graphics primitives). Link whichever of them this checkout provides.
set(HOG2_SOURCES "")
foreach(_f Colors.cpp Graphics.cpp)
  if(EXISTS ${HOG2_ROOT}/utils/${_f})
    list(APPEND HOG2_SOURCES ${HOG2_ROOT}/utils/${_f})
  endif()
endforeach()

add_executable(ballsort src/main.cpp ${HOG2_SOURCES})
target_include_directories(ballsort PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/environments
  ${HOG2_INCLUDE_DIRS}
)
target_compile_definitions(ballsort PRIVATE NO_OPENGL)
target_link_libraries(ballsort PRIVATE Threads::Threads)

# The experiment runner: one (algorithm, instance) pair per process, one CSV row out. One
# pair per process is what lets the sweep be a SLURM job array.
add_executable(run_experiment src/run_experiment.cpp ${HOG2_SOURCES})
target_include_directories(run_experiment PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/environments
  ${HOG2_INCLUDE_DIRS}
)
target_compile_definitions(run_experiment PRIVATE NO_OPENGL)
target_link_libraries(run_experiment PRIVATE Threads::Threads)

# Validation for the ported DFVS lower bound and the exact solver under it.
add_executable(dfvs_check src/test_dfvs.cpp ${HOG2_SOURCES})
target_include_directories(dfvs_check PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/environments
  ${HOG2_INCLUDE_DIRS}
)
target_compile_definitions(dfvs_check PRIVATE NO_OPENGL)

# Instance generation is pure combinatorics -- deliberately independent of HOG2 and of
# BallSort.h, so it gets no HOG2 include dirs and doesn't link against HOG2_SOURCES.
add_executable(generate_instances src/generate_instances.cpp)
target_include_directories(generate_instances PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

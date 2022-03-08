cmake_minimum_required(VERSION 2.6)

project(bytetrack)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

add_definitions(-std=c++11)

option(CUDA_USE_STATIC_CUDA_RUNTIME OFF)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_BUILD_TYPE Debug)

find_package(CUDA REQUIRED)
find_package(Eigen3 REQUIRED)

#-------------------------------------------------------------------------------
# Submodules
#-------------------------------------------------------------------------------

add_subdirectory(tkDNN)

#-------------------------------------------------------------------------------
# Includes
#-------------------------------------------------------------------------------

include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${EIGEN3_INCLUDE_DIR})
link_directories(${PROJECT_SOURCE_DIR}/include)
# include and link dirs of cuda and tensorrt, you need adapt them if yours are different

#-------------------------------------------------------------------------------
# CUDA
#-------------------------------------------------------------------------------
find_package(CUDA 9.0 REQUIRED)
SET(CUDA_SEPARABLE_COMPILATION ON)
set(CUDA_NVCC_FLAGS ${CUDA_NVCC_FLAGS} --maxrregcount=32)

find_package(CUDNN REQUIRED)
include_directories(${CUDNN_INCLUDE_DIR})
include_directories(${CUDA_INCLUDE_DIRS} ${NVINFER_INCLUDES})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tkDNN/include)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Ofast -O3 --fast-math -Wfatal-errors -D_MWAITXINTRIN_H_INCLUDED")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DTKPROJ_PATH='\"${CMAKE_CURRENT_SOURCE_DIR}/tkDNN/\"'")

find_package(OpenCV)
include_directories(${OpenCV_INCLUDE_DIRS})

file(GLOB My_Source_Files ${PROJECT_SOURCE_DIR}/src/*.cpp)
add_executable(bytetrack ${My_Source_Files})
target_link_libraries(bytetrack nvinfer nvinfer_plugin)
target_link_libraries(bytetrack cudart)
target_link_libraries(bytetrack tkDNN)
target_link_libraries(bytetrack ${OpenCV_LIBS})

add_definitions(-O2 -pthread)


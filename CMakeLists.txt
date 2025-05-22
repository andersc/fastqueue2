cmake_minimum_required(VERSION 3.22)
project(fast_queue2)

set(CMAKE_CXX_STANDARD 20)

find_package (Threads REQUIRED)

#Make sure we only target ARM/x86 64 bit architectures
if (CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
    message("Building target x86_64")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64)|(arm64)")
    message("Building target arm64")
else ()
    message("System CPU is: " ${CMAKE_SYSTEM_PROCESSOR})
    message( FATAL_ERROR "Not supported architecture, X86_64 and arm64 is." )
endif ()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/deaod_spsc/)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/dro/)

add_executable(fast_queue2 main.cpp)
target_link_libraries(fast_queue2 Threads::Threads)

add_executable(fast_queue_integrity_test FastQueueIntegrityTest.cpp)
target_link_libraries(fast_queue_integrity_test Threads::Threads)

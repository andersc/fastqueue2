cmake_minimum_required(VERSION 3.22)
project(fast_queue2)

set(CMAKE_CXX_STANDARD 20)

find_package(Threads REQUIRED)

# Make sure we only target ARM/x86 64-bit architectures.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(FQ_X86_64 TRUE)
    message("Building target x86_64")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    message("Building target arm64")
else ()
    message("System CPU is: " ${CMAKE_SYSTEM_PROCESSOR})
    message(FATAL_ERROR "Only x86_64 and arm64 are supported")
endif ()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/deaod_spsc/)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/dro/)

add_executable(fast_queue2 main.cpp)
target_link_libraries(fast_queue2 Threads::Threads)

add_executable(fast_queue_integrity_test FastQueueIntegrityTest.cpp)
target_link_libraries(fast_queue_integrity_test Threads::Threads)

if (FQ_X86_64)
    add_executable(fast_queue2_epyc EXCLUDE_FROM_ALL main.cpp)
    target_compile_definitions(fast_queue2_epyc PRIVATE
        FQ_EPYC_SLOT_SIGNAL=1
        FQ_SCALAR_BLOCKING=1
        FASTQUEUE_IMPLEMENTATION_LABEL="EPYC_slot-signaling")
    target_link_libraries(fast_queue2_epyc Threads::Threads)

    add_executable(fast_queue_epyc_integrity_test EXCLUDE_FROM_ALL FastQueueIntegrityTest.cpp)
    target_compile_definitions(fast_queue_epyc_integrity_test PRIVATE
        FQ_EPYC_SLOT_SIGNAL=1
        INTEGRITY_QUEUE_MASK=63)
    target_link_libraries(fast_queue_epyc_integrity_test Threads::Threads)

    set(ATOMIC_QUEUE_INCLUDE_DIR "" CACHE PATH
        "Path containing atomic_queue/atomic_queue.h for EPYC experiment")
    if (ATOMIC_QUEUE_INCLUDE_DIR)
        add_executable(fast_queue_epyc_experiment EXCLUDE_FROM_ALL FastQueueEpycExperiment.cpp)
        target_include_directories(fast_queue_epyc_experiment PRIVATE ${ATOMIC_QUEUE_INCLUDE_DIR})
        target_link_libraries(fast_queue_epyc_experiment Threads::Threads)
    endif()
endif()

add_executable(fast_queue_bulk_test FastQueueBulkTest.cpp)
target_link_libraries(fast_queue_bulk_test Threads::Threads)

add_executable(fast_queue_topology_matrix FastQueueTopologyMatrix.cpp)
target_link_libraries(fast_queue_topology_matrix Threads::Threads)

enable_testing()
add_test(NAME fast_queue_integrity_test COMMAND fast_queue_integrity_test)
set_tests_properties(fast_queue_integrity_test PROPERTIES TIMEOUT 240)
add_test(NAME fast_queue_bulk_test COMMAND fast_queue_bulk_test)
if (TARGET fast_queue_epyc_integrity_test)
    add_test(NAME fast_queue_epyc_integrity_test COMMAND fast_queue_epyc_integrity_test)
    set_tests_properties(fast_queue_epyc_integrity_test PROPERTIES TIMEOUT 240)
endif()

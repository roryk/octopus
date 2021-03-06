set(CONFIG_TEST_SOURCES
    
)

set(CONCEPTS_TEST_SOURCES
    concepts/comparable_tests.cpp
    concepts/mappable_tests.cpp
    concepts/mappable_range_tests.cpp
)

set(BASICS_TEST_SOURCES
    basics/contig_region_tests.cpp
    basics/genomic_region_tests.cpp
    basics/cigar_string_tests.cpp
    basics/aligned_read_tests.cpp
    basics/phred_tests.cpp
)

set(CONTAINERS_TEST_SOURCES
    containers/mappable_flat_set_tests.cpp
)

set(LOGGING_TEST_SOURCES
    
)

set(IO_TEST_SOURCES
    io/region_parser_tests.cpp
#    io/reference_genome_tests.cpp
)

set(READPIPE_TEST_SOURCES
    
)

set(UTILS_TEST_SOURCES
    utils/mappable_algorithm_tests.cpp
)

set(CORE_TEST_SOURCES
    core/types/allele_tests.cpp
    core/types/variant_tests.cpp
#    core/types/haplotype_tests.cpp
#    core/types/genotype_tests.cpp

    core/tools/global_aligner_tests.cpp
    core/tools/assembler_tests.cpp

    core/models/pair_hmm_tests.cpp
)

set(OCTOPUS_TEST_SOURCES
    unit_test_main.cpp
    ${CONFIG_TEST_SOURCES}
    ${CONCEPTS_TEST_SOURCES}
    ${BASICS_TEST_SOURCES}
    ${CONTAINERS_TEST_SOURCES}
    ${LOGGING_TEST_SOURCES}
    ${IO_TEST_SOURCES}
    ${READPIPE_TEST_SOURCES}
    ${UTILS_TEST_SOURCES}
    ${CORE_TEST_SOURCES}
)

find_package(SSE)
if (AVX2_FOUND)
    add_compile_options(-mavx2)
endif()

add_definitions(-DBOOST_TEST_DYN_LINK)
find_package(Boost 1.65 REQUIRED COMPONENTS unit_test_framework REQUIRED)

include_directories(${Boost_INCLUDE_DIRS} ${octopus_SOURCE_DIR}/lib ${octopus_SOURCE_DIR}/src ${octopus_SOURCE_DIR}/test)

set(TEST_DEPENDENCY_LIBS
    Octopus
    Mock
)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/build/cmake/modules/")

include(BoostTestHelpers)
# Add each test
foreach(SRC ${OCTOPUS_TEST_SOURCES})
    add_boost_test(${SRC} "${TEST_DEPENDENCY_LIBS}")
endforeach()

# add_executable(test_suite ${OCTOPUS_TEST_SOURCES})
# target_link_libraries(test_suite ${Boost_LIBRARIES})
# install(TARGETS test_suite DESTINATION ${octopus_SOURCE_DIR}/bin)

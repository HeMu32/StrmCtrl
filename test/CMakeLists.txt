# Collect all test source files
set(TEST_SOURCES
    main.cpp
    TestUtils.cpp
    StrmCtrlTest.cpp
)

add_executable(strmctrl_test ${TEST_SOURCES})

if(WIN32)
    target_compile_definitions(strmctrl_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_link_libraries(strmctrl_test PRIVATE
    strmctrl
)

add_test(NAME strmctrl_tests COMMAND strmctrl_test)

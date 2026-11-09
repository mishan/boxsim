# boxsim's tests, not a firmware: boxctl's reader, and examples/hello built
# for the linux target and run headless. Needs ESP-IDF's environment:
#   . ~/esp/esp-idf/export.sh && cmake -B build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(boxsim-tests NONE)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(IDF_PY idf.py REQUIRED)

set(hello "${CMAKE_BINARY_DIR}/hello")
add_test(NAME boxctl COMMAND Python3::Interpreter -m unittest tests/test_boxctl.py
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(boxctl PROPERTIES ENVIRONMENT "FAKEBOX_DIR=${CMAKE_BINARY_DIR}")
add_test(NAME hello-build
         COMMAND sh -c "[ -f '${hello}/sdkconfig' ] || '${IDF_PY}' --preview -B '${hello}' -D SDKCONFIG='${hello}/sdkconfig' set-target linux && '${IDF_PY}' -B '${hello}' -D SDKCONFIG='${hello}/sdkconfig' build"
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/examples/hello")
set_tests_properties(hello-build PROPERTIES FIXTURES_SETUP hello)
set(nocon "${CMAKE_BINARY_DIR}/hello-nocon")
add_test(NAME hello-nocon-build
         COMMAND sh -c "[ -f '${nocon}/sdkconfig' ] || '${IDF_PY}' --preview -B '${nocon}' -D SDKCONFIG='${nocon}/sdkconfig' -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;${CMAKE_SOURCE_DIR}/tests/sdkconfig.nocon' set-target linux && '${IDF_PY}' -B '${nocon}' -D SDKCONFIG='${nocon}/sdkconfig' build"
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/examples/hello")
set_tests_properties(hello-nocon-build PROPERTIES FIXTURES_SETUP hello)
add_test(NAME hello COMMAND Python3::Interpreter -m unittest tests/test_hello.py
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(hello PROPERTIES FIXTURES_REQUIRED hello
                     ENVIRONMENT "HELLO_ELF=${hello}/hello.elf;HELLO_NOCON_ELF=${nocon}/hello.elf;HELLO_OUT=${CMAKE_BINARY_DIR}")

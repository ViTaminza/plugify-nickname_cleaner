# nickname_cleaner — Plugify port of the Metamod plugin nickname_cleaner (ABKAM): rewrites
# player names that contain blocked words or advertisement with the configured default name.
#
#   plugify-nickname_cleaner  the plugin shared library (entry "bin/nickname_cleaner")
#   nickname_cleaner_core     the engine-independent filter/cleaning logic (+ its tests)

set(NICKNAME_CLEANER_VERSION "1.0.0")

# --- engine-independent core --------------------------------------------------
add_library(nickname_cleaner_core STATIC src/nickname_cleaner_core.cpp)
target_include_directories(nickname_cleaner_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(nickname_cleaner_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(nickname_cleaner_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-nickname_cleaner SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
)
target_include_directories(plugify-nickname_cleaner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated pisexcompat/plg/s2sdk headers must not fail -Werror on us.
target_include_directories(plugify-nickname_cleaner SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-nickname_cleaner PRIVATE
        nickname_cleaner_core
        pisexcompat
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-nickname_cleaner PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-nickname_cleaner PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="nickname_cleaner"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-nickname_cleaner PROPERTIES OUTPUT_NAME nickname_cleaner)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-nickname_cleaner PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/nickname_cleaner.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/nickname_cleaner.pplugin"
)

# --- tests --------------------------------------------------------------------
add_executable(nickname_cleaner_core_tests
        tests/nickname_cleaner_core_tests.cpp
        src/nickname_cleaner_core.cpp
)
target_include_directories(nickname_cleaner_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(nickname_cleaner_core_tests PRIVATE -Wall -Wextra -Werror)
# The list files the package ships drive the end-to-end checks.
target_compile_definitions(nickname_cleaner_core_tests PRIVATE
        NICKNAME_CLEANER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set_target_properties(nickname_cleaner_core_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

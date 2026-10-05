cmake_minimum_required(VERSION 3.19)
project(
        sts_lightspeed
        VERSION 0.1
        DESCRIPTION "High Performance, rng accurate implementation of Slay the Spire with tools for tree search and ml."
        LANGUAGES CXX)

file (GLOB_RECURSE sts_lightspeed_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

set(CMAKE_CXX_STANDARD 17)

if(MSVC)
    add_compile_options(/EHsc)
endif()

# GCC/Clang flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    string(APPEND CMAKE_CXX_FLAGS " -Wno-shift-count-overflow -O3")
elseif(MSVC)
    # MSVC: /O2 is equivalent to -O3 optimization level
    set(CMAKE_CXX_FLAGS_RELEASE "/O2")
endif()

add_subdirectory(json)
add_subdirectory(pybind11)

pybind11_add_module(slaythespire bindings/slaythespire.cpp bindings/bindings-util.cpp ${sts_lightspeed_SOURCES})
target_include_directories(slaythespire PUBLIC include)
target_include_directories(slaythespire PUBLIC json/single_include)
target_include_directories(slaythespire PUBLIC bindings)

# .pyi
if(WIN32)
    set(_STS_VENV_PY "${CMAKE_SOURCE_DIR}/.venv/Scripts/python.exe")
else()
    set(_STS_VENV_PY "${CMAKE_SOURCE_DIR}/.venv/bin/python")
endif()
if(EXISTS "${_STS_VENV_PY}")
    add_custom_command(
            TARGET slaythespire POST_BUILD
            COMMAND "${_STS_VENV_PY}" -m pybind11_stubgen slaythespire -o "$<TARGET_FILE_DIR:slaythespire>"
            COMMENT "refresh slaythespire.pyi (pybind11_stubgen)")
else()
    message(WARNING "python no found: ${_STS_VENV_PY}")
endif()


add_executable(main apps/main.cpp ${sts_lightspeed_SOURCES})
target_link_directories(main PRIVATE json::nlohmann_json)
target_include_directories(main PUBLIC include)
target_include_directories(main PUBLIC json/include)

add_executable(test apps/test.cpp ${sts_lightspeed_SOURCES})
target_include_directories(test PUBLIC include)
target_link_directories(test PUBLIC json::nlohmann_json)
target_include_directories(test PUBLIC json/include)

add_executable(small-test apps/small-test.cpp bindings/bindings-util.cpp ${sts_lightspeed_SOURCES})
target_link_directories(small-test PRIVATE json::nlohmann_json)
target_include_directories(small-test PUBLIC include)
target_include_directories(small-test PUBLIC json/include)
target_include_directories(small-test PUBLIC bindings)

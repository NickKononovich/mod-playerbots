#
# This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
# information; released under GNU GPL v2 license.
#
# [test1] The module's unit tests, as a project of their own.
#
# The framework is the CORE's googletest - the very same src/cmake/googletest.cmake the core's
# BUILD_TESTING path fetches, at the very same pinned commit. No second test framework exists.
#
# Why a project of its own and not only the core's unit_tests target: the core's unit_tests links
# `game` and `modules`, so building it means building the whole server (and switching BUILD_TESTING
# on in an existing build tree also appends the coverage flags to CMAKE_CXX_FLAGS, which rebuilds
# everything from scratch). The judge's seam deliberately depends on nothing but <cstdint>, so these
# tests compile in seconds on their own. The same sources are ALSO registered with the core's
# unit_tests through mod-playerbots.cmake, so a full -DBUILD_TESTING=ON core build runs them too -
# one set of tests, two ways to reach it.
#
# Configure and run (Windows, the toolchain the server is built with):
#   cmake -S tests -B build/tests -G "Visual Studio 17 2022" -A x64
#   cmake --build build/tests --config Release
#   ctest --test-dir build/tests -C Release --output-on-failure
#
cmake_minimum_required(VERSION 3.16)
project(pb_unit_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PB_MODULE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "mod-playerbots root")
set(PB_CORE_ROOT "C:/WoW/azerothcore" CACHE PATH "the custom core, for its googletest.cmake")

enable_testing()

# googletest defaults to the static MSVC runtime while everything else here uses the dynamic one; without this the
# link fails on libcpmt.lib against msvcprt.lib (LNK2038 RuntimeLibrary mismatch)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# the core's own googletest, pinned by the core
if(EXISTS "${PB_CORE_ROOT}/src/cmake/googletest.cmake")
    include("${PB_CORE_ROOT}/src/cmake/googletest.cmake")
    fetch_googletest("${PB_CORE_ROOT}/src/cmake" "${CMAKE_BINARY_DIR}/googletest")
else()
    message(FATAL_ERROR "the core's googletest.cmake was not found under ${PB_CORE_ROOT}: "
                        "point -DPB_CORE_ROOT at the custom core instead of adding a second framework")
endif()

add_executable(pb_unit_tests
    unit/Fixture.cpp
    unit/ChainPullJudgeTest.cpp
    unit/ChainPullCorpusTest.cpp
    unit/EstimateOutcomeTest.cpp
    unit/DeathReplayTest.cpp
    unit/IssueKeyTest.cpp
    unit/AggroReasonTest.cpp
    unit/SoloCombatEntryTest.cpp
    unit/StandingEpisodeTest.cpp
    unit/MoveStepTest.cpp
    unit/ChaseWindowTest.cpp
    unit/ManaBudgetTest.cpp
    unit/ActorFactsTest.cpp
    unit/RouteFactsTest.cpp
    unit/ManaLineTest.cpp
    unit/IdleWindowTest.cpp
    unit/AggroReachTest.cpp
    unit/ItemUseTest.cpp
    "${PB_MODULE_ROOT}/src/Ai/Leader/Fight/ChainPullJudge.cpp"
    "${PB_MODULE_ROOT}/src/Ai/Journal/IssueKey.cpp"
    "${PB_MODULE_ROOT}/src/Ai/Journal/ActorFacts.cpp"
    "${PB_MODULE_ROOT}/src/Ai/Leader/Travel/RouteFacts.cpp"
    "${PB_MODULE_ROOT}/src/Ai/Journal/AggroReason.cpp"
    "${PB_MODULE_ROOT}/src/Bot/Engine/SoloCombatEntry.cpp"
    "${PB_MODULE_ROOT}/src/Ai/Journal/StandingEpisode.cpp"
    "${PB_MODULE_ROOT}/src/Ai/Journal/IdleWindow.cpp"
    "${PB_MODULE_ROOT}/src/Ai/Base/Actions/MoveStep.cpp"
    "${PB_MODULE_ROOT}/src/Ai/World/Rpg/ChaseModel.cpp"
    "${PB_MODULE_ROOT}/src/Ai/World/Rpg/ManaBudget.cpp"
    "${PB_MODULE_ROOT}/src/Ai/World/Rpg/AggroReach.cpp"
    "${PB_MODULE_ROOT}/src/Ai/Base/Trigger/ManaLine.cpp")

target_include_directories(pb_unit_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/unit"
    "${PB_MODULE_ROOT}/src/Ai/Leader/Fight"
    "${PB_MODULE_ROOT}/src/Ai/Journal"
    "${PB_MODULE_ROOT}/src/Ai/Leader/Travel"
    "${PB_MODULE_ROOT}/src/Bot/Engine"
    "${PB_MODULE_ROOT}/src/Ai/Base/Actions"
    "${PB_MODULE_ROOT}/src/Ai/World/Rpg"
    "${PB_MODULE_ROOT}/src/Ai/Base/Trigger")

# the fixtures are files in the source tree, committed next to the tests; PB_FIXTURE_DIR overrides it
target_compile_definitions(pb_unit_tests PRIVATE
    PB_FIXTURE_DIR_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

target_link_libraries(pb_unit_tests PRIVATE gtest_main)

add_test(NAME pb_unit COMMAND pb_unit_tests)

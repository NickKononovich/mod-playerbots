# Included by the core's modules/CMakeLists.txt after the `modules` target exists.
#
# The core links the MySQL client library privately into its `database` target,
# so its include directories do not reach module code. PlayerbotsDatabase.cpp
# needs the complete MySQLPreparedStatement type (which pulls in mysql.h) and
# Playerbots.cpp uses ER_BAD_DB_ERROR from mysqld_error.h. Linking the imported
# `mysql` target here propagates those include directories to the module build.
target_link_libraries(modules
  PRIVATE
    mysql)

# [test1] The module's unit tests, registered with the core's own unit_tests target (src/test/CMakeLists.txt reads
# these two global properties). The framework is the core's googletest; no second one is added. The sources live in
# tests/ and NOT under src/, because the core sweeps every .cpp under a module's src/ into the module library -
# a gtest source there would be compiled into worldserver.
#
# The same sources also build on their own through tests/CMakeLists.txt, which is the fast way: the seam they test
# (src/Ai/Leader/Fight/ChainPullJudge.h) depends on nothing but <cstdint>, while unit_tests links `game` and
# `modules`, so reaching the tests this way means building the whole server.
if (BUILD_TESTING)
  set(PB_TESTS_DIR "${CMAKE_CURRENT_LIST_DIR}/tests")
  set(PB_FIXTURE_DIR "${PB_TESTS_DIR}/fixtures")
  configure_file(
    "${PB_TESTS_DIR}/unit/PbFixtureDir.h.in"
    "${CMAKE_BINARY_DIR}/pb_tests/PbFixtureDir.h"
    @ONLY)
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${PB_TESTS_DIR}/unit/Fixture.cpp"
    "${PB_TESTS_DIR}/unit/ChainPullJudgeTest.cpp"
    "${PB_TESTS_DIR}/unit/ChainPullCorpusTest.cpp"
    "${PB_TESTS_DIR}/unit/EstimateOutcomeTest.cpp"
    "${PB_TESTS_DIR}/unit/DeathReplayTest.cpp"
    "${PB_TESTS_DIR}/unit/IssueKeyTest.cpp"
    "${PB_TESTS_DIR}/unit/AggroReasonTest.cpp"
    "${PB_TESTS_DIR}/unit/SoloCombatEntryTest.cpp"
    "${PB_TESTS_DIR}/unit/StandingEpisodeTest.cpp"
    "${PB_TESTS_DIR}/unit/MoveStepTest.cpp"
    "${PB_TESTS_DIR}/unit/ChaseWindowTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_INCLUDES
    "${PB_TESTS_DIR}/unit"
    "${CMAKE_BINARY_DIR}/pb_tests")
endif()

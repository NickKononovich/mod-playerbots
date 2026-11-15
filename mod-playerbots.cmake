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
    "${PB_TESTS_DIR}/unit/ChaseWindowTest.cpp"
    # [jrn50] the journal's own instruments: the decision's participants, and a route as events. ManaBudgetTest is
    # in tests/CMakeLists.txt and was missing here before this line - the two lists had drifted apart.
    "${PB_TESTS_DIR}/unit/ManaBudgetTest.cpp"
    "${PB_TESTS_DIR}/unit/ActorFactsTest.cpp"
    "${PB_TESTS_DIR}/unit/RouteFactsTest.cpp"
    # [pull84] the pull's mana line: its seam compiles into the module from src, so only the test is listed here
    "${PB_TESTS_DIR}/unit/ManaLineTest.cpp"
    "${PB_TESTS_DIR}/unit/IdleWindowTest.cpp"
    # [death470] the ground one mob's aggro covers: its seam compiles into the module from src, so only the test here
    "${PB_TESTS_DIR}/unit/AggroReachTest.cpp"
    # [death475] was the item use accepted by the core: a header-only seam, so only the test here
    "${PB_TESTS_DIR}/unit/ItemUseTest.cpp"
    # [tele20] which copy of the dungeon a "go to him" lands in: a header-only seam, so only the test here
    "${PB_TESTS_DIR}/unit/GoToInstanceTest.cpp"
    # [mount9] the mount state's episode and its flip rule: its seam compiles into the module from src, so only
    # the test is listed here
    "${PB_TESTS_DIR}/unit/MountEpisodeTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_INCLUDES
    "${PB_TESTS_DIR}/unit"
    "${CMAKE_CURRENT_LIST_DIR}/src/Script"  # [tele20] GoToInstance.h, the header-only seam of "go to him"
    "${CMAKE_BINARY_DIR}/pb_tests")
endif()

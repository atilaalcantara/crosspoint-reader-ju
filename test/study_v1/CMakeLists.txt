add_executable(study_v1_test StudyV1Test.cpp StudyDeckTreeTest.cpp ../support/StudyFixtureBuilder.h
  ${REPO_ROOT}/src/study/StudyPackage.cpp
  ${REPO_ROOT}/src/study/StudyMedia.cpp
  ${REPO_ROOT}/src/study/StudyState.cpp
  ${REPO_ROOT}/src/study/StudyScheduler.cpp
  StudyMediaTest.cpp)
target_include_directories(study_v1_test PRIVATE ${REPO_ROOT}/src)
target_link_libraries(study_v1_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(study_v1_test)

add_executable(study_fixture_generator ../support/GenerateStudySimulatorFixture.cpp)
target_include_directories(study_fixture_generator PRIVATE ${REPO_ROOT}/src ${REPO_ROOT}/test/support)

add_executable(study_package_check ../support/StudyPackageCheck.cpp ${REPO_ROOT}/src/study/StudyPackage.cpp)
target_include_directories(study_package_check PRIVATE ${REPO_ROOT}/src)
target_link_libraries(study_package_check PRIVATE crosspoint_test_common)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME study_converter_integration
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../study_converter/test_converter.py
          --checker $<TARGET_FILE:study_package_check>)

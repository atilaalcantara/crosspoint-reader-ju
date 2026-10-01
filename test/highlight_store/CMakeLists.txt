add_executable(
  HighlightStoreTest
  HighlightStoreTest.cpp
  HighlightNoteUpdateTest.cpp
  HighlightSelectionTest.cpp
  ${REPO_ROOT}/src/util/HighlightStore.cpp
)
target_include_directories(HighlightStoreTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
)
target_link_libraries(HighlightStoreTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HighlightStoreTest)

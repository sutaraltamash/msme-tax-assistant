# Unit tests for the deterministic core. Each *_test.cpp is its own executable
# registered with CTest. Run with:  ctest --test-dir build -R '<name>'

set(TEST_SOURCES
  statematrix_test.cpp
  ruleengine_test.cpp
  gstrgenerator_test.cpp
  repository_test.cpp
  pipeline_test.cpp
  geminiclient_test.cpp
)

foreach(test_src ${TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)

  add_executable(${test_name} ${test_src})
  target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${test_name} PRIVATE tax_core)

  # Absolute source paths of the real config files, handed to tests that
  # exercise validateAgainstSchema (gemini_schema.json) and the RuleEngine
  # (rules_2026.json).
  target_compile_definitions(${test_name} PRIVATE
    GEMINI_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../config/gemini_schema.json"
    RULES_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../config/rules_2026.json"
  )

  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
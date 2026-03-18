cmake_minimum_required(VERSION 3.20)
project(quorum VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler warnings ──
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

# ── Dependencies ──
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)

# Find yaml-cpp (optional for now, will be required)
find_package(yaml-cpp QUIET)

# ── Sources ──
set(DAEMON_SOURCES
    src/main.cpp
    # Add .cpp files here as they are created
    # Header-only modules don't need listing
)

set(CLI_SOURCES
    src/cli/cli_main.cpp
    # src/cli/commands/init.cpp
    # src/cli/commands/agent.cpp
    # src/cli/commands/proposal.cpp
    # src/cli/commands/vault.cpp
    # src/cli/commands/daemon_cmd.cpp
    # src/cli/commands/audit.cpp
)

# ── Daemon executable ──
add_executable(quorum_daemon ${DAEMON_SOURCES})
target_include_directories(quorum_daemon PRIVATE src)
target_link_libraries(quorum_daemon PRIVATE
    CURL::libcurl
    OpenSSL::SSL OpenSSL::Crypto
    SQLite::SQLite3
)
if(yaml-cpp_FOUND)
    target_link_libraries(quorum_daemon PRIVATE yaml-cpp::yaml-cpp)
    target_compile_definitions(quorum_daemon PRIVATE HAS_YAML_CPP=1)
endif()

# ── CLI executable ──
# add_executable(quorum ${CLI_SOURCES})
# target_include_directories(quorum PRIVATE src)
# target_link_libraries(quorum PRIVATE
#     CURL::libcurl
#     OpenSSL::SSL OpenSSL::Crypto
#     SQLite::SQLite3
# )

# ── Tests ──
enable_testing()

# Unit tests (add as created)
# add_executable(test_scheduler tests/unit/test_scheduler.cpp)
# target_include_directories(test_scheduler PRIVATE src)
# add_test(NAME test_scheduler COMMAND test_scheduler)

add_executable(test_json tests/unit/test_json.cpp)
target_include_directories(test_json PRIVATE src)
add_test(NAME test_json COMMAND test_json)

add_executable(test_output_parser tests/unit/test_output_parser.cpp)
target_include_directories(test_output_parser PRIVATE src)
add_test(NAME test_output_parser COMMAND test_output_parser)

add_executable(test_handoff_parser tests/unit/test_handoff_parser.cpp)
target_include_directories(test_handoff_parser PRIVATE src)
add_test(NAME test_handoff_parser COMMAND test_handoff_parser)

add_executable(test_vault_manager tests/unit/test_vault_manager.cpp)
target_include_directories(test_vault_manager PRIVATE src)
add_test(NAME test_vault_manager COMMAND test_vault_manager)

add_executable(test_invoker tests/unit/test_invoker.cpp)
target_include_directories(test_invoker PRIVATE src)
add_test(NAME test_invoker COMMAND test_invoker)

add_executable(test_inbox_writer tests/unit/test_inbox_writer.cpp)
target_include_directories(test_inbox_writer PRIVATE src)
add_test(NAME test_inbox_writer COMMAND test_inbox_writer)

add_executable(test_database tests/unit/test_database.cpp)
target_include_directories(test_database PRIVATE src)
target_link_libraries(test_database PRIVATE SQLite::SQLite3)
add_test(NAME test_database COMMAND test_database)

add_executable(test_session_resume tests/unit/test_session_resume.cpp)
target_include_directories(test_session_resume PRIVATE src)
target_link_libraries(test_session_resume PRIVATE SQLite::SQLite3)
add_test(NAME test_session_resume COMMAND test_session_resume)

add_executable(test_knowledge_parser tests/unit/test_knowledge_parser.cpp)
target_include_directories(test_knowledge_parser PRIVATE src)
add_test(NAME test_knowledge_parser COMMAND test_knowledge_parser)

add_executable(test_knowledge_ledger tests/unit/test_knowledge_ledger.cpp)
target_include_directories(test_knowledge_ledger PRIVATE src)
target_link_libraries(test_knowledge_ledger PRIVATE SQLite::SQLite3)
add_test(NAME test_knowledge_ledger COMMAND test_knowledge_ledger)

add_executable(test_pipeline tests/integration/test_pipeline.cpp)
target_include_directories(test_pipeline PRIVATE src)
target_link_libraries(test_pipeline PRIVATE SQLite::SQLite3)
add_test(NAME test_pipeline COMMAND test_pipeline)

add_executable(test_generic_loop tests/unit/test_generic_loop.cpp)
target_include_directories(test_generic_loop PRIVATE src)
target_link_libraries(test_generic_loop PRIVATE SQLite::SQLite3)
add_test(NAME test_generic_loop COMMAND test_generic_loop)

add_executable(test_team_pipeline tests/integration/test_team_pipeline.cpp)
target_include_directories(test_team_pipeline PRIVATE src)
target_link_libraries(test_team_pipeline PRIVATE SQLite::SQLite3)
add_test(NAME test_team_pipeline COMMAND test_team_pipeline)

add_executable(test_roster_injection tests/unit/test_roster_injection.cpp)
target_include_directories(test_roster_injection PRIVATE src)
add_test(NAME test_roster_injection COMMAND test_roster_injection)

add_executable(test_config_validation tests/unit/test_config_validation.cpp)
target_include_directories(test_config_validation PRIVATE src)
add_test(NAME test_config_validation COMMAND test_config_validation)

add_executable(test_agent_create tests/unit/test_agent_create.cpp)
target_include_directories(test_agent_create PRIVATE src)
add_test(NAME test_agent_create COMMAND test_agent_create)

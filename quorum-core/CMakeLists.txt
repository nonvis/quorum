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
# macOS: Homebrew's OpenSSL is keg-only, so CMake's FindOpenSSL can't locate it
# without a hint. Query Homebrew for the prefix when OPENSSL_ROOT_DIR isn't set.
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
    find_program(BREW_EXECUTABLE brew)
    if(BREW_EXECUTABLE)
        execute_process(
            COMMAND ${BREW_EXECUTABLE} --prefix openssl@3
            OUTPUT_VARIABLE OPENSSL_ROOT_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
endif()

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

add_executable(test_vault_update_brainstorm tests/unit/test_vault_update_brainstorm.cpp)
target_include_directories(test_vault_update_brainstorm PRIVATE src)
add_test(NAME test_vault_update_brainstorm COMMAND test_vault_update_brainstorm)

add_executable(test_invoker tests/unit/test_invoker.cpp)
target_include_directories(test_invoker PRIVATE src)
add_test(NAME test_invoker COMMAND test_invoker)

add_executable(test_invoker_mode tests/unit/test_invoker_mode.cpp)
target_include_directories(test_invoker_mode PRIVATE src)
add_test(NAME test_invoker_mode COMMAND test_invoker_mode)

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

add_executable(test_pipeline tests/integration/test_pipeline.cpp)
target_include_directories(test_pipeline PRIVATE src)
target_link_libraries(test_pipeline PRIVATE SQLite::SQLite3)
add_test(NAME test_pipeline COMMAND test_pipeline)

add_executable(test_generic_loop tests/unit/test_generic_loop.cpp)
target_include_directories(test_generic_loop PRIVATE src)
target_link_libraries(test_generic_loop PRIVATE SQLite::SQLite3)
add_test(NAME test_generic_loop COMMAND test_generic_loop)

add_executable(test_brainstorm_doer_reject tests/unit/test_brainstorm_doer_reject.cpp)
target_include_directories(test_brainstorm_doer_reject PRIVATE src)
target_link_libraries(test_brainstorm_doer_reject PRIVATE SQLite::SQLite3)
add_test(NAME test_brainstorm_doer_reject COMMAND test_brainstorm_doer_reject)

add_executable(test_phase_plan_checkoff tests/unit/test_phase_plan_checkoff.cpp)
target_include_directories(test_phase_plan_checkoff PRIVATE src)
target_link_libraries(test_phase_plan_checkoff PRIVATE SQLite::SQLite3)
add_test(NAME test_phase_plan_checkoff COMMAND test_phase_plan_checkoff)

add_executable(test_context_history tests/unit/test_context_history.cpp)
target_include_directories(test_context_history PRIVATE src)
add_test(NAME test_context_history COMMAND test_context_history)

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

add_executable(test_quorum_init tests/unit/test_quorum_init.cpp)
target_include_directories(test_quorum_init PRIVATE src)
target_link_libraries(test_quorum_init PRIVATE SQLite::SQLite3)
add_test(NAME test_quorum_init COMMAND test_quorum_init)

add_executable(test_discover tests/unit/test_discover.cpp)
target_include_directories(test_discover PRIVATE src)
target_link_libraries(test_discover PRIVATE SQLite::SQLite3)
add_test(NAME test_discover COMMAND test_discover)

add_executable(test_agent_modify tests/unit/test_agent_modify.cpp)
target_include_directories(test_agent_modify PRIVATE src)
add_test(NAME test_agent_modify COMMAND test_agent_modify)

add_executable(test_agent_roster_dir tests/unit/test_agent_roster_dir.cpp)
target_include_directories(test_agent_roster_dir PRIVATE src)
add_test(NAME test_agent_roster_dir COMMAND test_agent_roster_dir)

add_executable(test_skill_discovery tests/unit/test_skill_discovery.cpp)
target_include_directories(test_skill_discovery PRIVATE src)
add_test(NAME test_skill_discovery COMMAND test_skill_discovery)

add_executable(test_crash_recovery tests/unit/test_crash_recovery.cpp)
target_include_directories(test_crash_recovery PRIVATE src)
target_link_libraries(test_crash_recovery PRIVATE SQLite::SQLite3)
add_test(NAME test_crash_recovery COMMAND test_crash_recovery)

# Phase 9 finding #2 — daemon-side hot reload of agents directory
add_executable(test_agent_hot_reload tests/unit/test_agent_hot_reload.cpp)
target_include_directories(test_agent_hot_reload PRIVATE src)
target_link_libraries(test_agent_hot_reload PRIVATE SQLite::SQLite3)
add_test(NAME test_agent_hot_reload COMMAND test_agent_hot_reload)

add_executable(test_assembler_rule_cap tests/unit/test_assembler_rule_cap.cpp)
target_include_directories(test_assembler_rule_cap PRIVATE src)
add_test(NAME test_assembler_rule_cap COMMAND test_assembler_rule_cap)

# Phase 7 Track 5 — system-prompt split for cache reuse
add_executable(test_assembler_split tests/unit/test_assembler_split.cpp)
target_include_directories(test_assembler_split PRIVATE src)
add_test(NAME test_assembler_split COMMAND test_assembler_split)

# Phase 9 Track 1 — ## Vault Inventory injection
add_executable(test_assembler_inventory tests/unit/test_assembler_inventory.cpp)
target_include_directories(test_assembler_inventory PRIVATE src)
add_test(NAME test_assembler_inventory COMMAND test_assembler_inventory)

# Phase 9 Track 2 — frontmatter tag parsing + scoring
add_executable(test_frontmatter_scoring tests/unit/test_frontmatter_scoring.cpp)
target_include_directories(test_frontmatter_scoring PRIVATE src)
add_test(NAME test_frontmatter_scoring COMMAND test_frontmatter_scoring)

# Phase 10 Track 3 — quorum vault dedup CLI
add_executable(test_vault_dedup tests/unit/test_vault_dedup.cpp)
target_include_directories(test_vault_dedup PRIVATE src)
add_test(NAME test_vault_dedup COMMAND test_vault_dedup)

# Phase 10 Track 4 — quorum vault audit CLI
add_executable(test_vault_audit tests/unit/test_vault_audit.cpp)
target_include_directories(test_vault_audit PRIVATE src)
add_test(NAME test_vault_audit COMMAND test_vault_audit)

# Phase 10 Track 5 — quorum converse --no-vault-write suppression flag
add_executable(test_no_vault_write tests/unit/test_no_vault_write.cpp)
target_include_directories(test_no_vault_write PRIVATE src)
target_link_libraries(test_no_vault_write PRIVATE SQLite::SQLite3)
add_test(NAME test_no_vault_write COMMAND test_no_vault_write)

# Phase 12: quorum ask CLI (ask the manager, read-only) — pure helpers
add_executable(test_ask tests/unit/test_ask.cpp)
target_include_directories(test_ask PRIVATE src)
add_test(NAME test_ask COMMAND test_ask)

# Phase 13 Track 3: quorum supervisor init — SUPERVISOR.md generator (autopilot)
add_executable(test_supervisor_init tests/unit/test_supervisor_init.cpp)
target_include_directories(test_supervisor_init PRIVATE src)
add_test(NAME test_supervisor_init COMMAND test_supervisor_init)

# Phase 14 Track 3: quorum knower refresh CLI — pure parts (arg validation,
# name validation, setup-gate, project-resolution reuse) + the generic-mode
# completion recommendation string (conversation.h → links SQLite).
add_executable(test_knower_refresh tests/unit/test_knower_refresh.cpp)
target_include_directories(test_knower_refresh PRIVATE src)
target_link_libraries(test_knower_refresh PRIVATE SQLite::SQLite3)
add_test(NAME test_knower_refresh COMMAND test_knower_refresh)

add_executable(test_invoker_cache_fields tests/unit/test_invoker_cache_fields.cpp)
target_include_directories(test_invoker_cache_fields PRIVATE src)
add_test(NAME test_invoker_cache_fields COMMAND test_invoker_cache_fields)

add_executable(test_invoker_sysprompt_flag tests/unit/test_invoker_sysprompt_flag.cpp)
target_include_directories(test_invoker_sysprompt_flag PRIVATE src)
add_test(NAME test_invoker_sysprompt_flag COMMAND test_invoker_sysprompt_flag)

# Phase 8 Track 2 / Track 6 #24 — rubric format + loader + resolver
add_executable(test_rubric_parser tests/unit/test_rubric_parser.cpp)
target_include_directories(test_rubric_parser PRIVATE src)
add_test(NAME test_rubric_parser COMMAND test_rubric_parser)

# Phase 8 Track 3 / Track 6 #26 — EVALUATION block parser
add_executable(test_evaluation_parser tests/unit/test_evaluation_parser.cpp)
target_include_directories(test_evaluation_parser PRIVATE src)
add_test(NAME test_evaluation_parser COMMAND test_evaluation_parser)

# Phase 8 Track 3 / Track 6 #27 — evaluator pipeline E2E
add_executable(test_evaluator_pipeline tests/integration/test_evaluator_pipeline.cpp)
target_include_directories(test_evaluator_pipeline PRIVATE src)
target_link_libraries(test_evaluator_pipeline PRIVATE SQLite::SQLite3)
add_test(NAME test_evaluator_pipeline COMMAND test_evaluator_pipeline)

# Phase 9 Track 6 — multi-conversation accumulation (#21) + inventory-aware
# update wiring (#23). Both cases share parser + VaultManager setup.
add_executable(test_inventory_write_side tests/integration/test_inventory_write_side.cpp)
target_include_directories(test_inventory_write_side PRIVATE src)
target_link_libraries(test_inventory_write_side PRIVATE SQLite::SQLite3)
add_test(NAME test_inventory_write_side COMMAND test_inventory_write_side)

# Phase 8 Track 5 / Track 6 #28 — rubric override integration test
add_executable(test_benchmark_runner tests/integration/test_benchmark_runner.cpp)
target_include_directories(test_benchmark_runner PRIVATE src)
target_link_libraries(test_benchmark_runner PRIVATE SQLite::SQLite3)
add_test(NAME test_benchmark_runner COMMAND test_benchmark_runner)

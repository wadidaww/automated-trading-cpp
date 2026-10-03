cmake_minimum_required(VERSION 3.25)

# Use vcpkg when available (VCPKG_ROOT), otherwise fall back to system packages.
if(NOT CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT}
   AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

project(futu_quant_trader VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FUTU_TRADER_BUILD_TESTS "Build tests" ON)
option(FUTU_TRADER_WERROR "Treat warnings as errors" OFF)
option(FUTU_TRADER_BUILD_BENCH "Build the futu_bench micro-benchmarks" OFF)

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Boost REQUIRED)  # header-only Boost.Asio
find_package(yaml-cpp REQUIRED)  # strict config parsing
find_package(OpenSSL REQUIRED)  # hardware-accelerated SHA-1 for the OpenD frame header

# Official Futu OpenD protobuf definitions, vendored and version-pinned (see PROTO_VERSION).
file(GLOB FUTU_PROTO_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/futu_proto/*.proto)
set(Protobuf_IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/futu_proto)
protobuf_generate_cpp(FUTU_PROTO_SRCS FUTU_PROTO_HDRS ${FUTU_PROTO_FILES})
add_library(futu_proto STATIC ${FUTU_PROTO_SRCS})  # generated code: no project warning flags
target_include_directories(futu_proto SYSTEM PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(futu_proto PUBLIC protobuf::libprotobuf)
target_compile_options(futu_proto PRIVATE -w)  # generated code; GCC 14 false-positive warnings

add_library(futu_trader
  src/instrument/hk_rules.cpp
  src/oms/order_state.cpp
  src/oms/pre_trade.cpp
  src/portfolio/fees.cpp
  src/sizing/sizing.cpp
  src/data/event_log.cpp
  src/infra/histogram.cpp
  src/infra/thread_tuning.cpp
  src/infra/metrics.cpp
  src/infra/wal.cpp
  src/infra/secrets.cpp
  src/app/metrics_bindings.cpp
  src/app/config.cpp
  src/app/application.cpp
  src/app/probe.cpp
  src/app/promotion.cpp
  src/infra/metrics_server.cpp
  src/data/csv.cpp
  src/data/validate.cpp
  src/backtest/synthetic.cpp
  src/backtest/metrics.cpp
  src/backtest/walk_forward.cpp
  src/backtest/sim_venue.cpp
  src/backtest/runner.cpp
  src/strategy/strategies.cpp
  src/strategy/oms_context.cpp
  src/engine/engine.cpp
  src/portfolio/position_book.cpp
  src/oms/live_gate.cpp
  src/oms/oms.cpp
  src/oms/journal_codec.cpp
  src/oms/push_router.cpp
  src/execution/rate_limiter.cpp
  src/execution/kill_switch.cpp
  src/opend/sha1.cpp
  src/opend/framing.cpp
  src/opend/connection.cpp
  src/opend/client.cpp
  src/opend/trade_decode.cpp
  src/opend/quote_decode.cpp
  src/api/futu_client.cpp
  src/data/data_normalizer.cpp
  src/data/market_data.cpp
  src/model/mean_reversion_model.cpp
  src/model/gradient_boosting_model.cpp
  src/model/model_trainer.cpp
  src/model/model_registry.cpp
  src/risk/risk_engine.cpp
  src/execution/order_manager.cpp
  src/execution/position_tracker.cpp
  src/pipeline/trading_pipeline.cpp
  src/evaluation/backtester.cpp
  src/evaluation/report_generator.cpp
  src/utils/config.cpp
)

target_include_directories(futu_trader PUBLIC include)
target_link_libraries(futu_trader PUBLIC futu_proto Threads::Threads Boost::headers)
target_link_libraries(futu_trader PRIVATE OpenSSL::Crypto yaml-cpp::yaml-cpp)
target_compile_options(futu_trader PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
if(FUTU_TRADER_WERROR)
  target_compile_options(futu_trader PRIVATE -Werror)
endif()

add_executable(futu_trader_app src/main.cpp)
target_link_libraries(futu_trader_app PRIVATE futu_trader)
set_target_properties(futu_trader_app PROPERTIES OUTPUT_NAME futu_trader)

add_executable(futu_backtest src/backtest_main.cpp)
target_link_libraries(futu_backtest PRIVATE futu_trader)

if(FUTU_TRADER_BUILD_BENCH)
  add_executable(futu_bench bench/futu_bench.cpp)
  target_link_libraries(futu_bench PRIVATE futu_trader)
endif()

add_executable(futu_model_train src/train_main.cpp)
target_link_libraries(futu_model_train PRIVATE futu_trader)

if(FUTU_TRADER_BUILD_TESTS)
  include(CTest)
  if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    include(GoogleTest)

    # Mock OpenD speaking the real wire protocol; test tool with fault injection.
    add_library(futu_mock_opend STATIC tools/mock_opend/mock_opend.cpp)
    target_include_directories(futu_mock_opend PUBLIC tools/mock_opend)
    target_link_libraries(futu_mock_opend PUBLIC futu_trader)

    add_executable(futu_unit_tests
      tests/unit/hk_rules_test.cpp
      tests/unit/order_state_test.cpp
      tests/unit/data_test.cpp
      tests/unit/ring_histogram_test.cpp
      tests/unit/engine_test.cpp
      tests/unit/metrics_test.cpp
      tests/unit/wal_test.cpp
      tests/unit/secrets_test.cpp
      tests/unit/config_test.cpp
      tests/unit/application_test.cpp
      tests/unit/promotion_test.cpp
      tests/unit/quote_decode_test.cpp
      tests/unit/oms_durable_test.cpp
      tests/unit/backtest_metrics_test.cpp
      tests/unit/sim_venue_test.cpp
      tests/unit/backtest_runner_test.cpp
      tests/unit/sizing_test.cpp
      tests/unit/oms_test.cpp
      tests/unit/oms_integration_test.cpp
      tests/unit/pre_trade_test.cpp
      tests/unit/portfolio_test.cpp
      tests/unit/safety_test.cpp
      tests/unit/opend_framing_test.cpp
      tests/unit/opend_client_test.cpp
      tests/unit/opend_trade_test.cpp
      tests/unit/data_normalizer_test.cpp
      tests/unit/model_test.cpp
      tests/unit/risk_test.cpp
      tests/unit/order_manager_test.cpp
      tests/unit/futu_client_test.cpp
      tests/unit/backtester_test.cpp
      tests/unit/pipeline_test.cpp
    )
    target_link_libraries(futu_unit_tests PRIVATE futu_trader futu_mock_opend GTest::gtest_main)
    target_compile_definitions(futu_unit_tests PRIVATE FUTU_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/config"
      FUTU_OPS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ops")
    gtest_discover_tests(futu_unit_tests)

    # Regression gate: a fixed synthetic backtest must reproduce the committed golden report
    # exactly, twice in a row (determinism), with the OMS agreeing with the simulated broker.
    add_test(NAME backtest_golden_meanrev
             COMMAND futu_backtest --synthetic --seed 42 --count 20000 --strategy meanrev
                     --check-determinism
                     --golden ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/backtest_meanrev_seed42.json)
    add_test(NAME backtest_golden_maker
             COMMAND futu_backtest --synthetic --seed 11 --count 20000 --size 200 --strategy maker
                     --max-daily-loss 900000 --check-determinism
                     --golden ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/backtest_maker_seed11.json)
    add_test(NAME backtest_lookahead_honest_strategy_passes
             COMMAND futu_backtest --synthetic --seed 5 --count 3000 --strategy meanrev
                     --check-lookahead)
    add_test(NAME backtest_lookahead_cheating_strategy_is_rejected
             COMMAND futu_backtest --synthetic --seed 5 --count 3000 --strategy peeker
                     --check-lookahead)
    set_tests_properties(backtest_lookahead_cheating_strategy_is_rejected PROPERTIES WILL_FAIL TRUE)
    add_test(NAME backtest_golden_random
             COMMAND futu_backtest --synthetic --seed 7 --count 20000 --strategy random
                     --check-determinism
                     --golden ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/backtest_random_seed7.json)

    # Allocation-counting tests replace global operator new, so they cannot be combined with the
    # sanitizers (which hook the allocator themselves).
    if(NOT CMAKE_CXX_FLAGS MATCHES "sanitize")
      add_executable(futu_alloc_tests tests/alloc/alloc_test.cpp)
      target_link_libraries(futu_alloc_tests PRIVATE futu_trader GTest::gtest_main)
      gtest_discover_tests(futu_alloc_tests)
    endif()

    add_executable(futu_e2e_tests tests/e2e/e2e_smoke_test.cpp)
    target_link_libraries(futu_e2e_tests PRIVATE GTest::gtest_main)
    target_compile_definitions(futu_e2e_tests PRIVATE
      FUTU_TRADER_BIN="$<TARGET_FILE:futu_trader_app>")
    add_dependencies(futu_e2e_tests futu_trader_app)
    gtest_discover_tests(futu_e2e_tests)
  endif()
endif()

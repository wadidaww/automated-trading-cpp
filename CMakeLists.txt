cmake_minimum_required(VERSION 3.25)
project(futu_quant_trader VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FUTU_TRADER_BUILD_TESTS "Build tests" ON)

add_library(futu_trader
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
target_compile_options(futu_trader PRIVATE -Wall -Wextra -Wpedantic)

add_executable(futu_trader_app src/main.cpp)
target_link_libraries(futu_trader_app PRIVATE futu_trader)
set_target_properties(futu_trader_app PROPERTIES OUTPUT_NAME futu_trader)

add_executable(futu_model_train src/train_main.cpp)
target_link_libraries(futu_model_train PRIVATE futu_trader)

if(FUTU_TRADER_BUILD_TESTS)
  include(CTest)
  if(BUILD_TESTING)
    add_executable(data_normalizer_test tests/unit/data_normalizer_test.cpp)
    target_link_libraries(data_normalizer_test PRIVATE futu_trader)
    add_test(NAME data_normalizer_test COMMAND data_normalizer_test)

    add_executable(model_test tests/unit/model_test.cpp)
    target_link_libraries(model_test PRIVATE futu_trader)
    add_test(NAME model_test COMMAND model_test)

    add_executable(risk_and_order_test tests/unit/risk_and_order_test.cpp)
    target_link_libraries(risk_and_order_test PRIVATE futu_trader)
    add_test(NAME risk_and_order_test COMMAND risk_and_order_test)

    add_executable(pipeline_backtester_test tests/unit/pipeline_backtester_test.cpp)
    target_link_libraries(pipeline_backtester_test PRIVATE futu_trader)
    add_test(NAME pipeline_backtester_test COMMAND pipeline_backtester_test)

    add_executable(futu_integration_tests tests/integration/pipeline_integration_test.cpp)
    target_link_libraries(futu_integration_tests PRIVATE futu_trader)
    add_test(NAME futu_integration_tests COMMAND futu_integration_tests)

    add_executable(futu_e2e_tests tests/e2e/e2e_smoke_test.cpp)
    target_link_libraries(futu_e2e_tests PRIVATE futu_trader)
    add_test(NAME futu_e2e_tests COMMAND futu_e2e_tests)
  endif()
endif()

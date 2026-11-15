add_executable(stock_video main.cpp)
target_compile_features(stock_video PRIVATE cxx_std_11)
target_include_directories(stock_video PRIVATE
  ../../lib/common/public ../../lib/vc5_common
  ../../lib/vc5_encoder ../../lib/vc5_decoder)
target_link_libraries(stock_video vc5_encoder vc5_decoder vc5_common common)

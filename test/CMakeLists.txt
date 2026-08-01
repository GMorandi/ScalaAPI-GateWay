add_executable(test_protocol unit/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE gateway_core)
add_test(NAME protocol_conversion COMMAND test_protocol)

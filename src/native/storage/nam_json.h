#pragma once
#include <cstddef>
#include <cstdint>
// Parses original .nam JSON into the engine's in-memory A2-Full representation.
// Does not modify the source file. Output is exactly 48,616 bytes.
bool pedalboard_parse_nam(const char* json, std::size_t length, std::uint8_t* output,
                          std::size_t capacity, char* error, std::size_t error_capacity);

#pragma once
#include <cstdint>
enum class HomeDestination : uint8_t { Library, Anki, Opds, Transfer, Tools, Settings };
constexpr HomeDestination kHomeDestinations[] = {HomeDestination::Library, HomeDestination::Anki,
                                                 HomeDestination::Opds,    HomeDestination::Transfer,
                                                 HomeDestination::Tools,   HomeDestination::Settings};

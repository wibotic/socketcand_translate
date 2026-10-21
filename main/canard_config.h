// Build configuration header for libcanard, included by canard.c through
// CANARD_CONFIG_HEADER (see main/CMakeLists.txt).
//
// libcanard 5.0.0 defines `static uint64_t random(canard_t*, uint64_t)`, which
// clashes with `long random(void)` from ESP-IDF's newlib <stdlib.h>.
// This header is included after the standard headers and before that
// definition, so it renames libcanard's function without editing the library.
//
// Remove this file and the matching lines in main/CMakeLists.txt once a
// libcanard release without the clash is required in main/idf_component.yml.
#define random canard_random

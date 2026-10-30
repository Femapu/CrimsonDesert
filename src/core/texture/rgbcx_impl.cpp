// QuattroStagioni - compiles the vendored rgbcx BC1/BC3/BC4/BC5 encoder (third_party/rgbcx, MIT or
// public domain, Rich Geldreich) as part of the core library.
//
// build.sh only compiles src/core/**.cpp, so the third-party translation unit is pulled in here.
// rgbcx only uses basic IEEE float operations (+ - * / fabs), so with -ffp-contract=off its output is
// the same on the native and the mingw build (checked by tests/test_texture.cpp hashes and by
// comparing qs_preview output under wine).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "rgbcx.cpp"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

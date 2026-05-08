# ============================================================
# CMakeLists.txt
# Compiles the native (C++) low-latency audio engine.
# Called by Gradle's externalNativeBuild block.
# ============================================================
cmake_minimum_required(VERSION 3.22.1)
project("hearingaid")

# ── Source files ──────────────────────────────────────────────
add_library(
    hearingaid         # Name of the resulting .so library
    SHARED             # Shared library loaded at runtime via System.loadLibrary()
    audio_engine.cpp   # Main AAudio stream manager + JNI bridge
    equalizer.cpp      # 10-band parametric IIR EQ
    noise_gate.cpp     # Noise gate / squelch to reduce background hiss
    compressor.cpp     # Dynamic range compressor (hearing loss compensation)
)

# ── Include paths ─────────────────────────────────────────────
target_include_directories(hearingaid PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ── Compiler flags ────────────────────────────────────────────
# -O3 -ffast-math: aggressive optimisation for the real-time callback
# -march=armv8-a+simd: enable NEON SIMD instructions on ARM64
target_compile_options(hearingaid PRIVATE
    -O3
    -ffast-math
    -march=armv8-a+simd
    -fno-exceptions           # Exceptions are slow in RT audio contexts
)

# ── System libraries ──────────────────────────────────────────
find_library(log-lib log)      # Android logging (logcat)
find_library(aaudio-lib aaudio)  # AAudio — Android's low-latency audio API

target_link_libraries(
    hearingaid
    ${log-lib}
    ${aaudio-lib}
)

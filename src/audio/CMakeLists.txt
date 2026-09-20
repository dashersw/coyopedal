set(COYOPEDAL_PEDAL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
include("${CMAKE_CURRENT_LIST_DIR}/block_frames.cmake")

idf_component_register(
    SRCS
    "${COYOPEDAL_PEDAL_ROOT}/src/audio/effects.cpp"
    "${COYOPEDAL_PEDAL_ROOT}/src/audio/processor.cpp"
    "${COYOPEDAL_PEDAL_ROOT}/src/audio/tuner.cpp"
    INCLUDE_DIRS
    "${COYOPEDAL_PEDAL_ROOT}/src"
    REQUIRES
    nam)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_definitions(${COMPONENT_LIB}
                           PUBLIC COYOPEDAL_PEDAL_BLOCK_FRAMES=${COYOPEDAL_PEDAL_BLOCK_FRAMES})
target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE -O3
            -fno-exceptions
            -fno-rtti
            -fno-math-errno
            -ffp-contract=off
            -Wall
            -Wextra
            -Wpedantic
            -Werror)

# Audio effect state is finite by construction and every user-facing parameter is clamped at
# configuration time. Let GCC remove NaN/signed-zero guard paths only from the effect translation
# unit; the A2-Full kernel and processor head keep strict arithmetic so they match the host model.
set_source_files_properties(
    "${COYOPEDAL_PEDAL_ROOT}/src/audio/effects.cpp"
    PROPERTIES
        COMPILE_OPTIONS
        "-funsafe-math-optimizations;-ffinite-math-only;-fno-signed-zeros;-fno-trapping-math;-ffp-contract=fast"
)

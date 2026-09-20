# The A2-Full engine and its ESP32-S3 kernels.
#
# The split layer is the first A2-Full layer that runs on core 1. At 8, core 0 runs layers 0-7 plus
# the USB completions and the pre-chain effects, and core 1 runs the remaining fifteen layers.
# Override it at configure time, e.g. -DCOYOPEDAL_PEDAL_S3_SPLIT_LAYER=9.
set(coyopedal_split_default "8")
# A CACHE default only applies to a build directory that has never seen the entry, so re-apply it
# whenever the default here changes.
if(NOT DEFINED COYOPEDAL_PEDAL_S3_SPLIT_LAYER_PINNED_DEFAULT
   OR NOT COYOPEDAL_PEDAL_S3_SPLIT_LAYER_PINNED_DEFAULT STREQUAL coyopedal_split_default)
    set(COYOPEDAL_PEDAL_S3_SPLIT_LAYER
        "${coyopedal_split_default}"
        CACHE STRING "First A2-Full layer assigned to the ESP32-S3 core 1 stage" FORCE)
    set(COYOPEDAL_PEDAL_S3_SPLIT_LAYER_PINNED_DEFAULT
        "${coyopedal_split_default}"
        CACHE INTERNAL "")
endif()
if(NOT COYOPEDAL_PEDAL_S3_SPLIT_LAYER MATCHES "^[1-9][0-9]*$" OR COYOPEDAL_PEDAL_S3_SPLIT_LAYER
                                                                 GREATER 22)
    message(FATAL_ERROR "COYOPEDAL_PEDAL_S3_SPLIT_LAYER must be a positive integer below 23")
endif()

idf_component_register(
    SRCS
    nam_a2_full_s3_native.cpp
    s3_quantize8_q24.S
    s3_native_fused_layer8.S
    s3_native_head16.S
    INCLUDE_DIRS
    .
    PRIV_REQUIRES
    esp_timer)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_definitions(${COMPONENT_LIB}
                           PUBLIC COYOPEDAL_PEDAL_S3_SPLIT_LAYER=${COYOPEDAL_PEDAL_S3_SPLIT_LAYER})
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
            -Wno-error=pedantic
            -Werror)

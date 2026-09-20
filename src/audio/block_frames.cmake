# One cache variable sets the DSP block size everywhere it is compiled. 64 is the production block.
# Override it at configure time, e.g. -DCOYOPEDAL_PEDAL_BLOCK_FRAMES=32.
set(COYOPEDAL_PEDAL_BLOCK_FRAMES
    "64"
    CACHE STRING "A2-Full DSP block size in audio frames")
set_property(CACHE COYOPEDAL_PEDAL_BLOCK_FRAMES PROPERTY STRINGS 8 16 32 48 64)

if(NOT COYOPEDAL_PEDAL_BLOCK_FRAMES MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "COYOPEDAL_PEDAL_BLOCK_FRAMES must be a positive integer")
endif()

math(EXPR COYOPEDAL_PEDAL_BLOCK_FRAMES_REMAINDER "${COYOPEDAL_PEDAL_BLOCK_FRAMES} % 2")
if(NOT COYOPEDAL_PEDAL_BLOCK_FRAMES_REMAINDER EQUAL 0)
    message(FATAL_ERROR "COYOPEDAL_PEDAL_BLOCK_FRAMES must be even for the two-frame S3 kernel")
endif()

if(COYOPEDAL_PEDAL_BLOCK_FRAMES GREATER 64)
    message(
        FATAL_ERROR "COYOPEDAL_PEDAL_BLOCK_FRAMES must not exceed the validated 64-frame maximum")
endif()

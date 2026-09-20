# TLSF's headers are private to ESP-IDF's own `heap` component, and only a component can ask for
# another component's directory — so the aligned-bank allocator that wraps tlsf_memalign_offs cannot
# be an app native source in `main`. It is its own component for that one reason.
idf_component_get_property(coyopedal_heap_dir heap COMPONENT_DIR)
idf_component_register(
    SRCS
    "bank_allocator.c"
    PRIV_INCLUDE_DIRS
    "${coyopedal_heap_dir}/tlsf/include"
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    PRIV_REQUIRES
    heap
    esp_system
    freertos
    nam)

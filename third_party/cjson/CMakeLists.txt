# cJSON 1.7.19, vendored so the pedal's NAM profile parser keeps its only
# external dependency without an IDF component-manager download. The board
# target comes from @geastack/targets and does not declare espressif/cjson.
idf_component_register(SRCS "cJSON.c" INCLUDE_DIRS ".")

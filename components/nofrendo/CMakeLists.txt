file(GLOB_RECURSE NOFRENDO_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/*.c")

idf_component_register(
    SRCS ${NOFRENDO_SRCS}
    INCLUDE_DIRS "src" "src/platform" "src/nes" "src/cpu" "src/sndhrdw" "src/libsnss"
    REQUIRES driver esp_lcd esp_driver_i2s esp_hw_support
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu17
    -Wno-implicit-function-declaration
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-char-subscripts
    -Wno-attributes
    -Wno-stringop-truncation
    -Wno-implicit-fallthrough
    -Wno-maybe-uninitialized
    -Wno-misleading-indentation
    -O2
)

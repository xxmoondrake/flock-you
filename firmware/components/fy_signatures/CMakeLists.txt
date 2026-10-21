idf_component_register(
    SRCS "fy_signatures.cpp" "dashboard_json.cpp"
    INCLUDE_DIRS "include"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)

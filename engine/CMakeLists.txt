# The Previously engine: pure C++20, no platform headers. Shared verbatim by every target.

add_library(previously_engine STATIC
    src/engine.cpp
)

add_library(previously::engine ALIAS previously_engine)

target_include_directories(previously_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

previously_apply_strict_options(previously_engine)
previously_apply_embedded_subset(previously_engine)

set_target_properties(previously_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_CROSSCOMPILING)
    # Host-only support: the contract violation handler (print and abort). Firmware supplies its own.
    add_library(previously_host_support STATIC
        src/host/contract_violation.cpp
    )

    add_library(previously::host_support ALIAS previously_host_support)

    # Only the contract header is needed; consumers link previously::engine before this library.
    target_include_directories(previously_host_support PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    previously_apply_strict_options(previously_host_support)
    previously_apply_embedded_subset(previously_host_support)

    set_target_properties(previously_host_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

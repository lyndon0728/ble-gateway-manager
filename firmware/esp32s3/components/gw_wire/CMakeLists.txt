# One implementation is shared with the nRF peer; no copied wire codec.
idf_component_register(SRCS "../../../nrf54l15/src/gw_core.c"
    "../../../nrf54l15/src/gw_filter_v2.c" INCLUDE_DIRS "include")

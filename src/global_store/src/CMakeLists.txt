SET(LINK_DEPS
	xstudio::json_store
	caf::core
)

SET(STATIC_LINK_DEPS
	xstudio::json_store_static
	caf::core
)

create_component_static(global_store 0.1.0 "${LINK_DEPS}" "${STATIC_LINK_DEPS}")

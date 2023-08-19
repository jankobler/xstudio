SET(LINK_DEPS
	xstudio::global_store
	caf::core
	dl
)

create_component(media_metadata 0.1.0 "${LINK_DEPS}")

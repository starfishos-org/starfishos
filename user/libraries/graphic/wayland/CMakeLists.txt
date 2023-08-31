add_subdirectory(dep/libffi)

add_library(wayland-core OBJECT
	src/connection.c
	src/wayland-os.c
	src/wayland-util.c)
target_include_directories(wayland-core PUBLIC include)
target_link_libraries(wayland-core PUBLIC ffi pipe)

add_library(wayland-server SHARED
	src/wayland-protocol.c
	src/wayland-server.c
	src/wayland-shm.c
	src/event-loop.c)
target_link_libraries(wayland-server PUBLIC wayland-core)

add_library(wayland-client SHARED
	src/wayland-protocol.c
	src/wayland-client.c)
target_link_libraries(wayland-client PUBLIC wayland-core)

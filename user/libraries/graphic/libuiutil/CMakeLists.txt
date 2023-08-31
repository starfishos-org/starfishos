file(
	GLOB
		uiutil_src
		"src/*.c"
)

add_library(uiutil SHARED ${uiutil_src})
target_include_directories(uiutil PUBLIC include)
target_link_libraries(uiutil PRIVATE pipe)

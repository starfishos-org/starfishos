file(
	GLOB
		graphic_src
		"src/*/*.c"
)

add_library(graphic SHARED ${graphic_src})
target_include_directories(graphic PUBLIC include)

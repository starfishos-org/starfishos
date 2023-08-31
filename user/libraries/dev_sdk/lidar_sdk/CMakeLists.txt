add_library(lidar_sdk SHARED lidar_sdk.c)
target_include_directories(lidar_sdk PUBLIC include)
target_link_libraries(lidar_sdk PUBLIC dm)

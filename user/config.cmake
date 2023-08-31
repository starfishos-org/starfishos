chcore_config(CHCORE_USER_DEBUG BOOL OFF "Build debug version of user-level libs and apps?")

chcore_config_include(libraries/config.cmake)
chcore_config_include(system-servers/config.cmake)
chcore_config_include(sample-apps/config.cmake)
chcore_config_include(demos/config.cmake)

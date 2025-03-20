chcore_config(CHCORE_USER_DEBUG BOOL ON "Build debug version of user-level libs and apps?")
chcore_config(CHCORE_BUILD_SAMPLE_APPS BOOL ON "Build sample apps?")
chcore_config(CHCORE_BUILD_DEMO_APPS BOOL ON "Build demo apps?")

chcore_config_include(libraries/config.cmake)
chcore_config_include(system-servers/config.cmake)
chcore_config_include(sample-apps/config.cmake)
chcore_config_include(demos/config.cmake)

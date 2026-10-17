find_package(Qt6 REQUIRED COMPONENTS Quick Qml Core DBus)

add_subdirectory(backend_mpv)

option(USE_LWE_SCENE
       "Render scene wallpapers with the linux-wallpaperengine backend" ON)
if(USE_LWE_SCENE)
  add_subdirectory(backend_lwe)
endif()

SET(BUILD_QML ON CACHE BOOL "")
add_subdirectory(backend_scene)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_library(${PROJECT_NAME}
	SHARED
	plugin.cpp
	MouseGrabber.cpp
	TTYSwitchMonitor.cpp
	PluginInfo.cpp
	FileHelper.cpp
	SteamPaths.cpp
	GamemodeMonitor.cpp
	WallpaperSyncBus.cpp
	GlobalConfigStore.cpp
	qmldir
)

target_link_libraries(${PROJECT_NAME} 
	PRIVATE
	Qt::Quick
	Qt::Qml 
	mpvbackend
	wescene-renderer-qml
	Qt::Core
	Qt::DBus
)

if(USE_LWE_SCENE)
  target_link_libraries(${PROJECT_NAME} PRIVATE lwebackend)
  target_compile_definitions(${PROJECT_NAME} PRIVATE WEKDE_HAVE_LWE_SCENE)
  # liblinux-wallpaperengine-core.so is produced inside the build tree by the
  # ExternalProject (lwe-install/), so the build tree binary finds it there.
  # The install rule in src/backend_lwe/ puts it into the system library
  # directory; the installed binary must look there instead, otherwise it would
  # depend on a build directory that no longer exists.
  set_target_properties(${PROJECT_NAME} PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lwe-install/lib"
    INSTALL_RPATH "${KDE_INSTALL_FULL_LIBDIR}")
endif()
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden)
set_target_properties(${PROJECT_NAME}
                       PROPERTIES LINK_FLAGS "-Wl,--exclude-libs,ALL")

# During plugin searching Qt will be looking for 'qmldir' file
# So we should place it next to our plugin lib.
add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND
        ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_LIST_DIR}/qmldir
		$<TARGET_FILE_DIR:${PROJECT_NAME}>/qmldir
)

install(
	TARGETS ${PROJECT_NAME}
    DESTINATION ${KDE_INSTALL_QMLDIR}/${QMLPLUGIN_INSTALL_URI}
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/qmldir
    DESTINATION ${KDE_INSTALL_QMLDIR}/${QMLPLUGIN_INSTALL_URI}
)


project(mpvbackend)

find_package(Qt${QT_MAJOR_VERSION} COMPONENTS Gui Quick OpenGL REQUIRED)
find_package(Qt${QT_MAJOR_VERSION}GuiPrivate REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED mpv)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_library(${PROJECT_NAME}
	STATIC
	MpvBackend.cpp
	qthelper.hpp
)
target_link_libraries(${PROJECT_NAME}
	PRIVATE
    Qt::Quick
	Qt::OpenGL
	Qt::GuiPrivate
	${MPV_LIBRARIES}
)
target_include_directories(${PROJECT_NAME} PUBLIC .)

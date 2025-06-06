project(ref_gl)

include_directories(${STB_INCLUDE_DIR})

if (USE_SDL2)
	include_directories(${SDL2_INCLUDE_DIR})
endif()

file(GLOB REF_GL_HEADERS
	"*.h"
	"../gameshared/q_*.h"
	"../gameshared/anorms.h"
	"../gameshared/config.h"
	"../qcommon/bsp.h"
	"../qcommon/patch.h"
	"../qcommon/iqm.h"
	"../qcommon/qfiles.h"
	"../qalgo/*.h"
	)

file(GLOB REF_GL_COMMON_SOURCES
	"*.c"
	"*.cpp"
	"../gameshared/q_*.c"
	"../qalgo/*.c"
	"../qcommon/bsp.c"
	"../qcommon/patch.c"
	)

if (USE_SDL2)
	file(GLOB REF_GL_PLATFORM_SOURCES
		"../sdl/sdl_glw.c"
		"../sdl/sdl_glw_icon.c"
		"../sdl/sdl_qgl.c"
		)

	set(REF_GL_PLATFORM_LIBRARIES ${SDL2_LIBRARY})
else()
	if (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
		file(GLOB REF_GL_PLATFORM_SOURCES
			"../win32/win_glw.c"
			"../win32/win_qgl.c"
			)

		set(REF_GL_PLATFORM_LIBRARIES "")
	endif()

	if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
		file(GLOB REF_GL_PLATFORM_SOURCES
			"../unix/unix_glw.c"
			"../unix/unix_qgl.c"
			"../unix/unix_xpm.c"
			)

		set(REF_GL_PLATFORM_LIBRARIES "X11" "Xext" "Xinerama" "Xrandr" "Xxf86vm" "m")
	endif()
endif()

add_library(ref_gl SHARED ${REF_GL_HEADERS} ${REF_GL_COMMON_SOURCES} ${REF_GL_PLATFORM_SOURCES})
target_link_libraries(ref_gl PRIVATE ${REF_GL_PLATFORM_LIBRARIES})
qf_set_output_dir(ref_gl libs)

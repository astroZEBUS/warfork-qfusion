project(snd_openal)

include_directories(${OPENAL_INCLUDE_DIR} ${STB_INCLUDE_DIR} "../snd_common" ".")

if (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    include_directories("../win32/include") # For al.h
endif()

file(GLOB SND_OPENAL_HEADERS
    "*.h"
	"../gameshared/q_*.h"
	"../client/snd_public.h"
)

file(GLOB SND_OPENAL_SOURCES
    "*.c"
    "../snd_common/*.c"
    "../gameshared/q_*.c"
)

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
        set(SND_OPENAL_PLATFORM_LIBRARIES "m" "dl")
else ()
        set(SND_OPENAL_PLATFORM_LIBRARIES "")
endif()

add_library(snd_openal SHARED ${SND_OPENAL_HEADERS} ${SND_OPENAL_SOURCES})
target_link_libraries(snd_openal PRIVATE ${SND_OPENAL_PLATFORM_LIBRARIES})
qf_set_output_dir(snd_openal libs)

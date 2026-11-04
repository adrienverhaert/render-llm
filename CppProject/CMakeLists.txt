cmake_minimum_required(VERSION 3.14.0)

# Build a 32-bit Windows executable
set(WINDOWS_32BIT 0)

# Build with release optimizations (Mac/Linux)
set(MAC_LINUX_RELEASE 1)

# Project setup
set(CMAKE_CXX_STANDARD 17)
set(QT_VERSION "5.15.9")
if (WIN32)
	set(CMAKE_CONFIGURATION_TYPES "Debug" "RelWithDebInfo" "Release")
	if (WINDOWS_32BIT)
		set(PLATFORM "Win32")
		set(WIN_EXT_FOLDER "Win32")
		set(QT_VERSION "${QT_VERSION}-Win32")
	else()
		set(PLATFORM "x64")
		set(WIN_EXT_FOLDER "Win64")
	endif()
	set(CMAKE_GENERATOR_PLATFORM ${PLATFORM})
elseif (NOT APPLE)
	set(CMAKE_C_COMPILER "/usr/bin/clang-12")
	set(CMAKE_CXX_COMPILER "/usr/bin/clang++-12")
endif()

project(render-llm)
include(CTest)
add_definitions("-DBUILD_FOLDER=QString(\"${CMAKE_BINARY_DIR}\")")
add_definitions("-DPROJECT_NAME=\"${PROJECT_NAME}\"")

# Platform specific configurations
if (WIN32)

	message("Windows platform (${PLATFORM})")
	add_definitions("-DOS_WINDOWS" /MP /bigobj /openmp)
	add_definitions("-DDEBUG_MODE=$<$<CONFIG:Debug>:1>$<$<CONFIG:RelWithDebInfo>:1>$<$<CONFIG:Release>:0>")
	add_definitions("-DRELEASE_MODE=!DEBUG_MODE")

	set(DEV_DIR "$ENV{DEV_DIR}" CACHE PATH "Headers for the pinned native dependencies")
	set(EXT_DIR "${CMAKE_SOURCE_DIR}/External/${WIN_EXT_FOLDER}")
	set(LIBS "${EXT_DIR}/zip.lib" "${EXT_DIR}/zlibstatic.lib" "${EXT_DIR}/OpenAL32.lib" "${EXT_DIR}/libx264.lib") # Zip, OpenAL, x264 (precompiled)
	set(LIBS ${LIBS} "Mf" "Mfplat" "mfuuid" "strmiids" "winmm") # FFmpeg and OpenAL dependencies
	# MI3: d3dcompiler linked in ALL configs — Release now self-compiles missing
	# shader blobs (self-healing cache) instead of shipping dead shaders.
	set(LIBS ${LIBS} "d3d11.lib" "d3dcompiler.lib") # Direct3D 11
	# MI3 Cinema M6 (fase A): backend DXR standalone (CinemaGpu12). d3d12.lib =
	# D3D12CreateDevice/D3D12SerializeRootSignature; dxgi.lib = CreateDXGIFactory2
	# (IDXGIFactory6::EnumAdapterByGpuPreference). dxc/dxil se cargan en runtime
	# (QLibrary, patron OIDN): NO se linka dxcompiler.lib.
	set(LIBS ${LIBS} "d3d12.lib" "dxgi.lib") # Direct3D 12 + DXGI 1.6 (backend DXR)
	set(APPICON_RC "${CMAKE_SOURCE_DIR}/Asset/Icons/appicon.rc")
	# MI3: CMAKE_MFC_FLAG removed — no source uses MFC (no afx*/CWinApp anywhere)
	# and requiring it breaks builds on toolchains without the MFC component.
	set_property(GLOBAL PROPERTY USE_FOLDERS ON)
	
	set(CMAKE_CXX_FLAGS_DEBUG "/ZI /Ob0 /Od /RTC1 /MT")
	set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/ZI /O2 /Ob1 /DNDEBUG /MT")
	# /Zi + /DEBUG: emit a PDB even in Release so WER crash dumps symbolize
	# (no runtime cost; the PDB stays in the build dir, not the install).
	set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ot /Ox /Zi /DNDEBUG /MT")
	set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
	set(CMAKE_CXX_FLAGS_DEBUG_WIN32 ${CMAKE_CXX_FLAGS_DEBUG})
	set(CMAKE_CXX_FLAGS_RELWITHDEBINFO_WIN32 ${CMAKE_CXX_FLAGS_RELWITHDEBINFO})
	set(CMAKE_CXX_FLAGS_RELEASE_WIN32 ${CMAKE_CXX_FLAGS_RELEASE})

else()

	set(DEV_DIR "~/Dev")
	add_definitions("-DOS_UNIX")
	add_compile_options("-Wall")
	add_compile_options("-Wno-overloaded-virtual")
	add_compile_options("-Wno-parentheses")
	add_compile_options("-Wno-sometimes-uninitialized")
	add_compile_options("-Wno-reorder-ctor")
	add_compile_options("-Wno-switch")
	add_compile_options("-Wno-unknown-pragmas")

	if (MAC_LINUX_RELEASE)
		add_compile_options("-O3")
		add_compile_options("-DNDEBUG")
		add_definitions("-DDEBUG_MODE=0")
		add_definitions("-DRELEASE_MODE=1")
	else()
		add_compile_options("-O0")
		add_compile_options("-g")
		add_definitions("-DDEBUG_MODE=1")
		add_definitions("-DRELEASE_MODE=0")
	endif()

	if(APPLE)

		message("Mac platform")
		add_definitions("-DOS_MAC")
		set(EXT_DIR "${CMAKE_SOURCE_DIR}/External/Mac")
	
		# FFmpeg dependencies
		set(LIBS ${LIBS} "iconv")
		set(LIBS ${LIBS} "-framework AudioToolBox")
		set(LIBS ${LIBS} "-framework VideoToolBox")
		set(LIBS ${LIBS} "-framework CoreMedia")
		set(LIBS ${LIBS} "-framework CoreAudio")
		set(LIBS ${LIBS} "-framework VideoDecodeAcceleration")
		set(LIBS ${LIBS} "${EXT_DIR}/libx264.a")
	
	else()

		message("Linux platform")
		set(EXT_DIR "${CMAKE_SOURCE_DIR}/External/Linux")
		set(LIBS ${LIBS} "-lvdpau -lva -lva-drm -lva-x11 -lsndio -lx264 -lgnutls -lnettle")
		set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath='$ORIGIN'/Data") # Find .so in Data folder

	endif()

	set(LIBS ${LIBS} "${EXT_DIR}/libz.a" "${EXT_DIR}/libzip.a" "${EXT_DIR}/libopenal.a") # Zip, OpenAL (precompiled)
	set(LIBS ${LIBS} "bz2" "lzma") # Zip dependencies
	
	# Add OpenMP
	find_package(OpenMP REQUIRED)
	set(LIBS ${LIBS} OpenMP::OpenMP_CXX)

endif()

message("DEV_DIR = ${DEV_DIR}")
message("EXT_DIR = ${EXT_DIR}")

# Add Qt
set(Qt_DIR "${DEV_DIR}/Qt/${QT_VERSION}/build")
message("Qt_DIR = ${Qt_DIR}")
# Preserve package prefixes supplied by a toolchain (notably vcpkg). The legacy
# DEV_DIR Qt build remains a fallback for existing developer workstations.
list(PREPEND CMAKE_PREFIX_PATH ${Qt_DIR})
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)
find_package(Qt5 COMPONENTS Widgets Gui Network REQUIRED)
find_package(Freetype REQUIRED)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${Qt5Widgets_EXECUTABLE_COMPILE_FLAGS}")
add_definitions(${Qt5Widgets_DEFINITIONS})
set(LIBS ${LIBS} Qt5::Widgets Qt5::Network Freetype::Freetype)

# Find sprite and shader assets and create a project
file(GLOB_RECURSE ASSETS RELATIVE "${CMAKE_SOURCE_DIR}" "*.png" "*.vsh" "*.fsh" "*.d3d")
# Keep the original source images for provenance while omitting retired MI-3
# onboarding, advertising, upgrade, asset-browser and watermark art from the
# native render-llm executable. Runtime UI glyphs and renderer noise stay embedded.
list(FILTER ASSETS EXCLUDE REGEX
	"^Asset/Sprites/(spr_bench_example|spr_jonathan_splash|spr_load_assets|spr_logo|spr_missing_thumbnail|spr_modelbench_ad|spr_path_example|spr_unknown_pack|spr_upgrade_img|spr_watermark)_frame_[0-9]+\\.png$")
source_group(TREE "${CMAKE_SOURCE_DIR}/Asset" FILES ${ASSETS})
add_custom_target(Assets SOURCES ${ASSETS})

# Generate index.qrc file to embed assets into executable
set(INDEX_XML "<!-- Note: Auto-generated, do not modify! -->\n<RCC>\n\t<qresource prefix=\"/\">")
foreach(CURRENT_ASSET ${ASSETS})
	string(REGEX REPLACE "Asset/" "" CURRENT_ASSET "${CURRENT_ASSET}")
	set(INDEX_XML "${INDEX_XML}\n\t\t<file>${CURRENT_ASSET}</file>")
endforeach()
set(INDEX_XML "${INDEX_XML}\n\t</qresource>\n</RCC>")

set(QT_INDEX_FILE "${CMAKE_SOURCE_DIR}/Asset/index.qrc")
file(REMOVE ${QT_INDEX_FILE})
file(WRITE ${QT_INDEX_FILE} "${INDEX_XML}")
add_definitions(-DASSETS_DIR="${CMAKE_SOURCE_DIR}/Asset")

# Add external libraries
set(INCLUDE_DIRS ${CMAKE_SOURCE_DIR})
set(INCLUDE_DIRS ${INCLUDE_DIRS} ${Qt5Widgets_INCLUDE_DIRS})
set(INCLUDE_DIRS ${INCLUDE_DIRS} "${CMAKE_SOURCE_DIR}/External")
set(INCLUDE_DIRS ${INCLUDE_DIRS} "${DEV_DIR}/Libzip/libzip-1.9.2/lib")
set(INCLUDE_DIRS ${INCLUDE_DIRS} "${DEV_DIR}/FreeType/freetype-2.9.1/include")
set(INCLUDE_DIRS ${INCLUDE_DIRS} "${DEV_DIR}/FFmpeg/ffmpeg-5.0")
set(INCLUDE_DIRS ${INCLUDE_DIRS} "${DEV_DIR}/OpenAL/openal-soft-1.22.0/include")

# Add FFmpeg (precompiled)
set(LIBS ${LIBS} "${EXT_DIR}/libavformat.a")
set(LIBS ${LIBS} "${EXT_DIR}/libavcodec.a")
set(LIBS ${LIBS} "${EXT_DIR}/libswresample.a")
set(LIBS ${LIBS} "${EXT_DIR}/libswscale.a")
set(LIBS ${LIBS} "${EXT_DIR}/libavutil.a")

# Add C++ sources
file(GLOB_RECURSE SRCS RELATIVE "${CMAKE_SOURCE_DIR}" "*.cpp" "*.hpp")
list(FILTER SRCS EXCLUDE REGEX "^Tests/")
source_group(TREE "${CMAKE_SOURCE_DIR}" PREFIX ${PROJECT_NAME} FILES ${SRCS})
qt5_add_resources(SRCS ${QT_INDEX_FILE})

# Create executable
add_executable(${PROJECT_NAME} ${SRCS} ${APPICON_RC})
if(MSVC)
	# Use our main with the matching static CRT, not Qt's shared-CRT WinMain shim.
	set_property(TARGET ${PROJECT_NAME} PROPERTY Qt5_NO_LINK_QTMAIN TRUE)
	target_link_options(${PROJECT_NAME} PRIVATE /ENTRY:mainCRTStartup)
endif()
target_precompile_headers(${PROJECT_NAME} PUBLIC "Common.hpp")
target_include_directories(${PROJECT_NAME} PUBLIC ${INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} ${LIBS})

option(RENDER_LLM_ASAN "Instrument native memory accesses (diagnostic builds only)" OFF)
if(RENDER_LLM_ASAN)
	if(NOT MSVC)
		message(FATAL_ERROR "The native ASan diagnostic currently targets MSVC on Windows.")
	endif()
	# Existing Debug flags use /RTC1 and /ZI, which are incompatible with ASan.
	set(CMAKE_CXX_FLAGS_DEBUG "/Zi /Od /MT")
	set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/Zi /O2 /DNDEBUG /MT")
	# Prebuilt Qt uses unannotated STL containers. Keep the ODR consistent while
	# retaining ASan heap/stack/use-after-free instrumentation in our native sources.
	target_compile_definitions(${PROJECT_NAME} PRIVATE _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
	target_compile_options(${PROJECT_NAME} PRIVATE /fsanitize=address)
	target_link_options(${PROJECT_NAME} PRIVATE /INCREMENTAL:NO)
	get_filename_component(MSVC_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
	set(ASAN_RUNTIME "${MSVC_BIN}/clang_rt.asan_dynamic-x86_64.dll")
	if(NOT EXISTS "${ASAN_RUNTIME}")
		message(FATAL_ERROR "Missing installed MSVC ASan runtime: ${ASAN_RUNTIME}")
	endif()
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ASAN_RUNTIME}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
		VERBATIM)
endif()

if (BUILD_TESTING)
	add_executable(RenderLlmDocumentTests
		Tests/RenderLlmDocumentTests.cpp
		Native/RenderLlmDocument.cpp
		Native/RenderLlmDocument.hpp
	)
	target_include_directories(RenderLlmDocumentTests PRIVATE ${CMAKE_SOURCE_DIR})
	target_link_libraries(RenderLlmDocumentTests PRIVATE Qt5::Core)
	add_test(NAME renderllm-document COMMAND RenderLlmDocumentTests)
endif()

# Project file settings
set_property(TARGET ${PROJECT_NAME} PROPERTY WIN32_EXECUTABLE "$<$<CONFIG:Debug>:0>$<$<CONFIG:Release>:1>")
set_directory_properties(PROPERTIES VS_STARTUP_PROJECT ${PROJECT_NAME})
if (WIN32 AND WINDOWS_32BIT)
	set_target_properties(${PROJECT_NAME} PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release-Win32"
		RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release-Win32"
	)
elseif (APPLE)
	set_target_properties(${PROJECT_NAME} PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
		RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
	)
endif()

# windows specific target definitions
set_target_properties(sunshine PROPERTIES LINK_SEARCH_START_STATIC 1)

if(NOT WINUHID_RUNTIME_DIR)
    set(_winuhid_guess "$ENV{USERPROFILE}/src/WinUHid/build/Release/x64")
    if(EXISTS "${_winuhid_guess}/WinUHidDevs.dll")
        set(WINUHID_RUNTIME_DIR "${_winuhid_guess}")
    endif()
endif()
if(WINUHID_RUNTIME_DIR)
    add_custom_command(TARGET sunshine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WINUHID_RUNTIME_DIR}/WinUHid.dll"
            "${WINUHID_RUNTIME_DIR}/WinUHidDevs.dll"
            $<TARGET_FILE_DIR:sunshine>
            COMMENT "Copy WinUHid runtime DLLs next to sunshine.exe")
endif()
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")
find_library(ZLIB ZLIB1)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        $<TARGET_OBJECTS:sunshine_rc_object>
        Windowsapp.lib
        Wtsapi32.lib
        version.lib)

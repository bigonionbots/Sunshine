# linux specific target definitions

if(NOT FREEBSD)
    # Using newer c++ compilers / features on older distros causes runtime dyn link errors
    list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
            -static-libgcc
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
        # GCC 15's static libstdc++ does not provide the out-of-line wait/notify
        # symbol used by std::stop_token, so libstdc++ must be a direct DSO input.
        list(APPEND SUNSHINE_EXTERNAL_LIBRARIES stdc++)
    else()
        list(APPEND SUNSHINE_EXTERNAL_LIBRARIES -static-libstdc++)
    endif()
endif()

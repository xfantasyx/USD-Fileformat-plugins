#[=======================================================================[.rst:
----

Finds the OpenImageIO library.
This find module will simply redirect to a find_package(CONFIG) hinted to look
into the pxr_ROOT.


Imported Targets
^^^^^^^^^^^^^^^^

This module provides the following imported targets, if found:

``OpenImageIO::OpenImageIO``
  The OpenImageIO library

Result Variables
^^^^^^^^^^^^^^^^

This will define the following variables:

``OpenImageIO_FOUND``
  True if the system has the OpenImageIO library.



#]=======================================================================]
if (TARGET OpenImageIO::OpenImageIO AND TARGET OpenImageIO::OpenImageIO_Util)
    return()
endif()

if(${OpenImageIO_FIND_REQUIRED})
    find_package(OpenImageIO PATHS ${pxr_ROOT} ${pxr_ROOT}/lib64 REQUIRED)
else()
    find_package(OpenImageIO PATHS ${pxr_ROOT} ${pxr_ROOT}/lib64)
endif()

# Ensure both OpenImageIO and OpenImageIO_Util targets are available
if(NOT TARGET OpenImageIO::OpenImageIO_Util AND TARGET OpenImageIO::OpenImageIO)
    # Sometimes the Util library is named differently, try to find it
    get_target_property(OIIO_LOCATION OpenImageIO::OpenImageIO LOCATION)
    get_filename_component(OIIO_DIR ${OIIO_LOCATION} DIRECTORY)
    
    # Look for the util library in the same directory
    find_library(OIIO_UTIL_LIB 
        NAMES OpenImageIO_Util libOpenImageIO_Util
        PATHS ${OIIO_DIR}
        NO_DEFAULT_PATH
    )
    
    if(OIIO_UTIL_LIB)
        add_library(OpenImageIO::OpenImageIO_Util UNKNOWN IMPORTED)
        set_target_properties(OpenImageIO::OpenImageIO_Util PROPERTIES
            IMPORTED_LOCATION ${OIIO_UTIL_LIB}
        )
    endif()
endif()

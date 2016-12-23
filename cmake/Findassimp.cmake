# - Find ASSIMP
# Find the ASSIMP includes and library
#
#  ASSIMP_INCLUDE_DIR - Where to find ASSIMP includes
#  ASSIMP_LIBRARIES   - List of libraries when using ASSIMP
#  ASSIMP_FOUND       - True if ASSIMP was found.

IF (ASSIMP_INCLUDE_DIR)
  SET(ASSIMP_FIND_QUIETLY TRUE)
ENDIF (ASSIMP_INCLUDE_DIR)

FIND_PATH(ASSIMP_INCLUDE_DIR postprocess.h
  PATHS
  $ENV{ASSIMP_HOME}/include
  $ENV{EXTERNLIBS}/ASSIMP/include
  /usr/local/include
  /usr/include
  DOC "ASSIMP - Headers"
  NO_DEFAULT_PATH
  PATH_SUFFIXES ASSIMP
)
FIND_PATH(ASSIMP_INCLUDE_DIR postprocess.h DOC "ASSIMP - Headers" PATH_SUFFIXES ASSIMP)

SET(ASSIMP_NAMES assimp assimp-vc140-mt)
SET(ASSIMP_DBG_NAMES assimp assimp-vc140-mtd)

FIND_LIBRARY(ASSIMP_LIBRARY NAMES ${ASSIMP_NAMES}
  PATHS
  $ENV{ASSIMP_HOME}/lib
  $ENV{EXTERNLIBS}/ASSIMP/lib
  DOC "ASSIMP - Library"
  #NO_DEFAULT_PATH
  PATH_SUFFIXES lib lib64
)
FIND_LIBRARY(ASSIMP_LIBRARY NAMES ${ASSIMP_NAMES} DOC "ASSIMP - Library")


INCLUDE(FindPackageHandleStandardArgs)

IF(MSVC)
  # VisualStudio needs a debug version
  FIND_LIBRARY(ASSIMP_LIBRARY_DEBUG NAMES ${ASSIMP_DBG_NAMES}
    PATHS
    $ENV{ASSIMP_HOME}/lib
    $ENV{EXTERNLIBS}/ASSIMP/lib
    DOC "ASSIMP - Library (Debug)"
  )
  
  IF(ASSIMP_LIBRARY_DEBUG AND ASSIMP_LIBRARY)
    SET(ASSIMP_LIBRARIES optimized ${ASSIMP_LIBRARY} debug ${ASSIMP_LIBRARY_DEBUG})
  ENDIF(ASSIMP_LIBRARY_DEBUG AND ASSIMP_LIBRARY)

  FIND_PACKAGE_HANDLE_STANDARD_ARGS(ASSIMP DEFAULT_MSG ASSIMP_LIBRARY ASSIMP_LIBRARY_DEBUG ASSIMP_INCLUDE_DIR)

  MARK_AS_ADVANCED(ASSIMP_LIBRARY ASSIMP_LIBRARY_DEBUG ASSIMP_INCLUDE_DIR)
  
ELSE(MSVC)
  # rest of the world
  SET(ASSIMP_LIBRARIES ${ASSIMP_LIBRARY})

  FIND_PACKAGE_HANDLE_STANDARD_ARGS(ASSIMP DEFAULT_MSG ASSIMP_LIBRARY ASSIMP_INCLUDE_DIR)
  
  MARK_AS_ADVANCED(ASSIMP_LIBRARY ASSIMP_INCLUDE_DIR)
  
ENDIF(MSVC)

IF(ASSIMP_FOUND)
  SET(ASSIMP_INCLUDE_DIRS ${ASSIMP_INCLUDE_DIR})
ENDIF(ASSIMP_FOUND)

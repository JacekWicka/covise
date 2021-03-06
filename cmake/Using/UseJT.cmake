MACRO(USE_JT)
  FIND_PACKAGE(JT)
  IF ((NOT JT_FOUND) AND (${ARGC} LESS 1))
    USING_MESSAGE("Skipping because of missing JT")
    RETURN()
  ENDIF((NOT JT_FOUND) AND (${ARGC} LESS 1))
  IF(NOT JT_USED AND JT_FOUND)
    SET(JT_USED TRUE)
    INCLUDE_DIRECTORIES(${JT_INCLUDE_DIR})
    SET(EXTRA_LIBS ${EXTRA_LIBS} ${JT_LIBRARIES})
  ENDIF()
ENDMACRO(USE_JT)

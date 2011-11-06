
macro (add_openmw_dir dir)
set (files)
foreach (u ${ARGN})
file (GLOB ALL RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "${dir}/${u}.*")
foreach (f ${ALL})
list (APPEND files "${f}")
list (APPEND OPENMW_FILES "${f}")
endforeach (f)
endforeach (u)
source_group ("apps\\openmw\\${dir}" FILES ${files})
endmacro (add_openmw_dir)

macro (add_component_dir dir)
set (files)
foreach (u ${ARGN})
file (GLOB ALL RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "${dir}/${u}.*")
foreach (f ${ALL})
list (APPEND files "${f}")
list (APPEND COMPONENT_FILES "${f}")
endforeach (f)
endforeach (u)
source_group ("components\\${dir}" FILES ${files})
endmacro (add_component_dir)

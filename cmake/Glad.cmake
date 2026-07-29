# glad2 generates its OpenGL loader from the Khronos XML registry at configure
# time using a Python script that depends on Jinja2. To keep the build a single
# `cmake -B build` with no manual prerequisites, we locate a Python 3 that can
# import jinja2 and, failing that, provision a throwaway venv inside the build
# tree and install jinja2 into it.

FetchContent_Declare(glad
    URL           https://github.com/Dav1dde/glad/archive/refs/tags/v2.0.8.tar.gz
    URL_HASH      SHA256=44f06f9195427c7017f5028d0894f57eb216b0a8f7c4eda7ce883732aeb2d0fc
    SOURCE_SUBDIR cmake)
FetchContent_MakeAvailable(glad)

function(_plant_provision_glad_python out_var)
    find_program(PLANT_HOST_PYTHON NAMES python3 python
        DOC "Python 3 interpreter used to run the glad OpenGL loader generator")
    if(NOT PLANT_HOST_PYTHON)
        message(FATAL_ERROR
            "Python 3 is required to generate the glad OpenGL loader but was not found on PATH.")
    endif()

    execute_process(COMMAND "${PLANT_HOST_PYTHON}" -c "import jinja2"
        RESULT_VARIABLE has_jinja OUTPUT_QUIET ERROR_QUIET)
    if(has_jinja EQUAL 0)
        set(${out_var} "${PLANT_HOST_PYTHON}" PARENT_SCOPE)
        return()
    endif()

    set(venv "${CMAKE_BINARY_DIR}/glad-python")
    if(WIN32)
        set(venv_python "${venv}/Scripts/python.exe")
    else()
        set(venv_python "${venv}/bin/python")
    endif()

    if(NOT EXISTS "${venv_python}")
        message(STATUS "glad: jinja2 unavailable, creating venv at ${venv}")
        execute_process(COMMAND "${PLANT_HOST_PYTHON}" -m venv "${venv}"
            RESULT_VARIABLE rc OUTPUT_QUIET)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "glad: failed to create Python venv at ${venv} (exit ${rc})")
        endif()
        execute_process(
            COMMAND "${venv_python}" -m pip install --quiet --disable-pip-version-check jinja2
            RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "glad: failed to install jinja2 into ${venv} (exit ${rc})")
        endif()
    endif()

    set(${out_var} "${venv_python}" PARENT_SCOPE)
endfunction()

# glad_add_library() calls find_package(Python COMPONENTS Interpreter) and then
# invokes ${Python_EXECUTABLE}; seeding the cache entries pins both the FindPython
# and FindPython3 spellings to the interpreter we validated above.
_plant_provision_glad_python(PLANT_GLAD_PYTHON)
set(Python_EXECUTABLE  "${PLANT_GLAD_PYTHON}" CACHE FILEPATH "Python 3 for glad codegen" FORCE)
set(Python3_EXECUTABLE "${PLANT_GLAD_PYTHON}" CACHE FILEPATH "Python 3 for glad codegen" FORCE)
message(STATUS "glad: generating OpenGL 3.3 core loader with ${PLANT_GLAD_PYTHON}")

# REPRODUCIBLE pins the generated sources so the build is deterministic.
glad_add_library(glad_gl_core_33 STATIC REPRODUCIBLE API gl:core=3.3)

# Sistema di build alternativo agli script bash (build_hx.sh/build_inspect.sh), che restano
# invariati per chi preferisce non installare CMake (code review: Codex, fase 3 - "unificare le
# build con CMake o Meson, aggiungere target build/test/install, mantenere gli script attuali
# come semplici wrapper se utili agli utenti meno esperti").
#
# Uso:
#   cmake -B build && cmake --build build && ctest --test-dir build
#     -> costruisce solo il modulo annexb (nessuna dipendenza dall'SDK NDI) e i suoi test.
#
#   cmake -B build -DNDI_SDK_DIR=/path/to/ndi-adv-sdk && cmake --build build
#     -> costruisce anche ndi_hx_send e ndi_hx_inspect, collegati contro l'SDK indicato.
#        Se la cartella lib/ dell'SDK contiene piu' sottocartelle per architetture diverse,
#        quella giusta viene scelta in base a CMAKE_SYSTEM_PROCESSOR; per bypassare la
#        selezione automatica passa anche -DNDI_SDK_LIBDIR=/path/esatto.

cmake_minimum_required(VERSION 3.16)
project(rodecaster-ndi-hx CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# --- Modulo annexb: nessuna dipendenza dall'SDK NDI, sempre costruito -------------------------
add_library(annexb STATIC annexb.cpp annexb.h)
target_include_directories(annexb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- Test (CTest) -------------------------------------------------------------------------------
enable_testing()
add_executable(test_annexb tests/test_annexb.cpp)
target_link_libraries(test_annexb PRIVATE annexb)
target_compile_options(test_annexb PRIVATE -Wconversion -Wsign-conversion -Werror)
add_test(NAME annexb_unit_tests COMMAND test_annexb)

# --- ndi_hx_send / ndi_hx_inspect: solo se viene indicato l'SDK NDI Advanced -------------------
# L'SDK e' proprietario e non e' incluso in questo repo (vedi README): senza -DNDI_SDK_DIR=...
# `cmake --build` costruisce comunque annexb e i test, semplicemente salta questi due target.
if(DEFINED NDI_SDK_DIR)
    if(NOT EXISTS "${NDI_SDK_DIR}/include")
        message(FATAL_ERROR "NDI_SDK_DIR='${NDI_SDK_DIR}' non contiene una cartella include/")
    endif()

    if(DEFINED NDI_SDK_LIBDIR)
        set(_ndi_libdir "${NDI_SDK_LIBDIR}")
    else()
        file(GLOB _ndi_lib_candidates LIST_DIRECTORIES true "${NDI_SDK_DIR}/lib/*")
        set(_ndi_libdir "")
        set(_ndi_libdir_fallback "")
        foreach(_cand ${_ndi_lib_candidates})
            if(IS_DIRECTORY "${_cand}")
                if(_ndi_libdir_fallback STREQUAL "")
                    set(_ndi_libdir_fallback "${_cand}")
                endif()
                get_filename_component(_cand_name "${_cand}" NAME)
                if(_cand_name MATCHES "${CMAKE_SYSTEM_PROCESSOR}")
                    set(_ndi_libdir "${_cand}")
                    break()
                endif()
            endif()
        endforeach()
        if(_ndi_libdir STREQUAL "")
            # Nessuna sottocartella corrisponde all'host: se ce n'e' una sola in assoluto usiamo
            # comunque quella (caso comune, nessuna ambiguita' possibile), altrimenti falliamo
            # con un messaggio chiaro invece di indovinare.
            list(LENGTH _ndi_lib_candidates _n_candidates)
            if(_n_candidates EQUAL 1)
                set(_ndi_libdir "${_ndi_libdir_fallback}")
            else()
                message(FATAL_ERROR
                    "SDK multi-architettura: nessuna sottocartella lib/ corrisponde "
                    "all'host (${CMAKE_SYSTEM_PROCESSOR}). Trovate: ${_ndi_lib_candidates}. "
                    "Imposta -DNDI_SDK_LIBDIR=... per sceglierne una esplicitamente.")
            endif()
        endif()
    endif()
    message(STATUS "NDI Advanced SDK: include=${NDI_SDK_DIR}/include lib=${_ndi_libdir}")

    foreach(_target ndi_hx_send ndi_hx_inspect)
        if(_target STREQUAL "ndi_hx_send")
            add_executable(${_target} ${_target}.cpp)
            target_link_libraries(${_target} PRIVATE annexb)
        else()
            add_executable(${_target} ${_target}.cpp)
        endif()
        target_include_directories(${_target} PRIVATE "${NDI_SDK_DIR}/include")
        target_link_directories(${_target} PRIVATE "${_ndi_libdir}")
        target_link_libraries(${_target} PRIVATE ndi_advanced dl pthread)
        set_target_properties(${_target} PROPERTIES
            INSTALL_RPATH "${_ndi_libdir}"
            BUILD_WITH_INSTALL_RPATH TRUE)
    endforeach()

    install(TARGETS ndi_hx_send ndi_hx_inspect RUNTIME DESTINATION bin)
else()
    message(STATUS "NDI_SDK_DIR non impostato: verranno costruiti solo il modulo annexb e i "
                    "test. Passa -DNDI_SDK_DIR=/path/to/ndi-adv-sdk anche per ndi_hx_send/"
                    "ndi_hx_inspect.")
endif()

install(PROGRAMS capture_cam_hx.sh DESTINATION bin)
install(FILES ndi-webcam-send.service DESTINATION share/rodecaster-ndi-hx)

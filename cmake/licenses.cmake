# Keep license notices beside the app in both build trees and installed packages.
set(STYX_LICENSE_DIR "${CMAKE_CURRENT_BINARY_DIR}/styx-licenses")
file(REMOVE_RECURSE "${STYX_LICENSE_DIR}")
file(MAKE_DIRECTORY "${STYX_LICENSE_DIR}")

function(styx_license name source)
    configure_file("${source}" "${STYX_LICENSE_DIR}/${name}.txt" COPYONLY)
endfunction()

styx_license(emoji-segmenter "${CMAKE_CURRENT_SOURCE_DIR}/vendor/emoji-segmenter/LICENSE")
styx_license(rmlui-backend "${CMAKE_CURRENT_SOURCE_DIR}/vendor/rmlui-backend/LICENSE.txt")
styx_license(lucide "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/lucide/LICENSE")
if(ENABLE_VOICE)
    styx_license(libdave "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libdave/LICENSE")
    styx_license(mlspp "${CMAKE_CURRENT_SOURCE_DIR}/vendor/mlspp/LICENSE")
endif()
if(ENABLE_VOICE OR TARGET ffmpeg)
    styx_license(miniaudio "${CMAKE_CURRENT_SOURCE_DIR}/vendor/miniaudio/LICENSE")
endif()
if(ENABLE_RNNOISE)
    styx_license(rnnoise "${CMAKE_CURRENT_SOURCE_DIR}/vendor/rnnoise/COPYING")
endif()
if(NOT TARGET unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator)
    # The fallback's MIT notice is embedded in the public header.
    styx_license(qrcodegen "${CMAKE_CURRENT_SOURCE_DIR}/vendor/qrcodegen/cpp/qrcodegen.hpp")
endif()

file(GLOB _styx_external_notices CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/licenses/*.txt")
foreach(_notice IN LISTS _styx_external_notices)
    get_filename_component(_name "${_notice}" NAME)
    configure_file("${_notice}" "${STYX_LICENSE_DIR}/${_name}" COPYONLY)
endforeach()

if(USE_VCPKG)
    file(GLOB _styx_vcpkg_notices CONFIGURE_DEPENDS
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*/copyright")
    if(NOT _styx_vcpkg_notices)
        message(FATAL_ERROR "No vcpkg dependency license notices were found.")
    endif()
    foreach(_notice IN LISTS _styx_vcpkg_notices)
        get_filename_component(_package "${_notice}" DIRECTORY)
        get_filename_component(_package "${_package}" NAME)
        styx_license("vcpkg/${_package}" "${_notice}")
    endforeach()
endif()

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.txt" DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${STYX_LICENSE_DIR}/" DESTINATION "${CMAKE_INSTALL_BINDIR}/licenses")

cmake_minimum_required(VERSION 3.20)
project(singularity-shell VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# Minimum supported: Qt 6.5 (QWebEngineUrlScheme flags, QWebEngineScript worlds)
find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    WebEngineCore
    WebEngineWidgets
    WebChannel
    LinguistTools
)
qt_standard_project_setup()

# Install prefix for the data dir (script, baseline assets in the RPM).
include(GNUInstallDirs)

set(SINGULARITY_SHELL_DATADIR "${CMAKE_INSTALL_FULL_DATADIR}/singularity-shell")

qt_add_executable(singularity-shell
    src/main.cpp
    src/AssetStore.h
    src/AssetStore.cpp
    src/SgSchemeHandler.h
    src/SgSchemeHandler.cpp
    src/NavigationPage.h
    src/NavigationPage.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/PreloadBridge.h
    src/PreloadBridge.cpp
    src/UpdateController.h
    src/UpdateController.cpp
    src/DiagnosticsInterceptor.h
    src/VendorApiInterceptor.h
    resources/shell.qrc
)

target_compile_definitions(singularity-shell PRIVATE
    SINGULARITY_SHELL_DATADIR="${SINGULARITY_SHELL_DATADIR}"
    SINGULARITY_SHELL_SYSTEM_ASSETS="${SINGULARITY_SHELL_DATADIR}/assets"
    APP_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(singularity-shell PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::WebEngineCore
    Qt6::WebEngineWidgets
    Qt6::WebChannel
)
# --- Translations: compile .ts → .qm (TS is hand-maintained) -------------------
# Kept in i18n/ (non-standard) so qt_standard_project_setup does not auto-run
# lupdate and overwrite the hand-edited file. qt_add_lrelease only compiles;
# the tool path is auto-discovered via the already-found Qt6LinguistTools.
qt_add_lrelease(singularity-shell
    TS_FILES i18n/singularity-shell_ru.ts
    QM_FILES_OUTPUT_VARIABLE qm_files
)
install(FILES ${qm_files}
        DESTINATION ${SINGULARITY_SHELL_DATADIR}/translations)

# --- asar-extract: tiny dependency-free CLI used by fetch-assets.sh ----------
add_executable(asar-extract tools/asar-extract.cpp)

install(TARGETS singularity-shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS asar-extract      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS scripts/fetch-assets.sh
        DESTINATION ${SINGULARITY_SHELL_DATADIR})
install(FILES packaging/singularity-shell.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
# Baseline assets are installed by the RPM build (see packaging/*.spec):
#   ${SINGULARITY_SHELL_DATADIR}/assets/<version>/...

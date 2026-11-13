# kiems-gui: the cross-platform KiEMS desktop app (Qt 6 Widgets). The macOS-only Swift app in
# KiEMS/ is separate and unaffected; this one builds wherever Qt does. It reads and edits the same
# .g2esim document packages, and runs simulations through the kiems command-line tool, which it
# expects beside its own executable.

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Charts Concurrent OpenGL OpenGLWidgets)
qt_standard_project_setup()

qt_add_executable(kiems-gui WIN32 MACOSX_BUNDLE
    main.cpp
    BoardModel.cpp
    BoardModel.hpp
    BoardSetupPanel.cpp
    BoardSetupPanel.hpp
    BoardView.cpp
    BoardView.hpp
    ChartStyle.hpp
    Document.cpp
    Document.hpp
    EngineSettings.cpp
    EngineSettings.hpp
    EyeTestPresets.cpp
    EyeTestPresets.hpp
    EyeView.cpp
    EyeView.hpp
    FieldViewer.cpp
    FieldViewer.hpp
    GeometryScene.cpp
    GeometryScene.hpp
    GeometryView.cpp
    GeometryView.hpp
    Icons.cpp
    Icons.hpp
    Inspector.cpp
    Inspector.hpp
    JobQueue.cpp
    JobQueue.hpp
    MainWindow.cpp
    MainWindow.hpp
    PipelineRunner.cpp
    PipelineRunner.hpp
    PowerAnalysis.cpp
    PowerAnalysis.hpp
    PowerView.cpp
    PowerView.hpp
    QuantityEdit.cpp
    QuantityEdit.hpp
    Report.cpp
    Report.hpp
    ResultsView.cpp
    ResultsView.hpp
    RunEstimate.cpp
    RunEstimate.hpp
    SelectionPanel.cpp
    SelectionPanel.hpp
    SettingsPanel.cpp
    SettingsPanel.hpp
    Sidebar.cpp
    Sidebar.hpp
    SignalSetup.cpp
    SignalSetup.hpp
    SimulationEditor.cpp
    SimulationEditor.hpp
    SimulationSet.cpp
    SimulationSet.hpp
    SimulationSetPanel.cpp
    SimulationSetPanel.hpp
    StatusDisplay.cpp
    StatusDisplay.hpp
    TestsPanel.cpp
    TestsPanel.hpp
    TraceAnalysis.cpp
    TraceAnalysis.hpp
    TraceView.cpp
    TraceView.hpp)

# The in-app guide (Help > Simulating a Signal).
qt_add_resources(kiems-gui "guide" PREFIX "/guide" BASE "guide" FILES "guide/SimulatingASignal.md")

set_target_properties(kiems-gui PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    MACOSX_BUNDLE_BUNDLE_NAME "KiEMS")
target_include_directories(kiems-gui PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/libkiems"
    "${PROJECT_SOURCE_DIR}/submodules")
# Copper for its field frame-series reader (the Field Viewer).
target_link_libraries(kiems-gui PRIVATE libkiems libkicad Copper Qt6::Widgets Qt6::Charts Qt6::Concurrent
    Qt6::OpenGL Qt6::OpenGLWidgets)

# The app runs the pipeline through the command-line tools, found beside its own executable.
add_dependencies(kiems-gui kiems-cli kiems_fdtd_worker copper_fdtd_worker)

if(WIN32)
    if(MSVC)
        target_sources(kiems-gui PRIVATE "${PROJECT_SOURCE_DIR}/cmake/windows/kiems.manifest")
    endif()
    # Qt's plugins (the Windows platform plugin, styles, image formats) must sit beside the
    # executable, which vcpkg's DLL copying doesn't cover; windeployqt does.
    if(TARGET Qt6::windeployqt)
        add_custom_command(TARGET kiems-gui POST_BUILD
            COMMAND Qt6::windeployqt --no-translations --no-compiler-runtime --no-system-d3d-compiler
                    --no-opengl-sw "$<TARGET_FILE:kiems-gui>"
            COMMENT "Deploying Qt runtime files next to kiems-gui"
            VERBATIM)
    endif()
endif()

# The GUI's editing logic, tested through its widgets without showing a window. Built on request
# (`cmake --build ... --target kiems-gui-tests`), and registered with CTest when testing is on.
find_package(Qt6 6.5 QUIET COMPONENTS Test)
if(TARGET Qt6::Test)
    qt_add_executable(kiems-gui-tests
        tests/kiems_gui_tests.cpp
        BoardModel.cpp
        BoardModel.hpp
        BoardSetupPanel.cpp
        BoardSetupPanel.hpp
        Document.cpp
        Document.hpp
        EyeTestPresets.cpp
        EyeTestPresets.hpp
        EyeView.cpp
        EyeView.hpp
        GeometryScene.cpp
        GeometryScene.hpp
        GeometryView.cpp
        GeometryView.hpp
        JobQueue.cpp
        JobQueue.hpp
        PowerAnalysis.cpp
        PowerAnalysis.hpp
        PowerView.cpp
        PowerView.hpp
        QuantityEdit.cpp
        QuantityEdit.hpp
        Report.cpp
        Report.hpp
        ResultsView.cpp
        ResultsView.hpp
        RunEstimate.cpp
        RunEstimate.hpp
        SettingsPanel.cpp
        SettingsPanel.hpp
        SignalSetup.cpp
        SignalSetup.hpp
        SimulationEditor.cpp
        SimulationEditor.hpp
        SimulationSet.cpp
        SimulationSet.hpp
        SimulationSetPanel.cpp
        SimulationSetPanel.hpp
        TestsPanel.cpp
        TestsPanel.hpp
        TraceAnalysis.cpp
        TraceAnalysis.hpp
        TraceView.cpp
        TraceView.hpp)
    set_target_properties(kiems-gui-tests PROPERTIES
        EXCLUDE_FROM_ALL TRUE
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO)
    target_include_directories(kiems-gui-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/libkiems"
        "${PROJECT_SOURCE_DIR}/submodules")
    target_link_libraries(kiems-gui-tests PRIVATE libkiems libkicad Qt6::Widgets Qt6::Charts Qt6::Concurrent
        Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Test)
    if(BUILD_TESTING)
        add_test(NAME kiems-gui-tests COMMAND kiems-gui-tests -platform offscreen)
    endif()
endif()

install(TARGETS kiems-gui
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    BUNDLE DESTINATION .)

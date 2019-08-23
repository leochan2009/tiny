add_library(furry_gui_cvwindow window_QT.cpp window_QT.h window_QT.qrc)
target_link_libraries(furry_gui_cvwindow ${OpenCV_LIBS} ${QT_LIBRARIES})

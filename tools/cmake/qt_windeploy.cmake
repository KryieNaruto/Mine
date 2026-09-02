# qt_windeploy(<target>):Windows 上给 Qt 可执行目标加 windeployqt POST_BUILD。
#
# 背景:Qt DLL 在 .user-deps/qt/<ver>/msvc2019_64/bin,VS 启动进程继承的是 VS 自己的环境
# (不含 env.sh 里的 PATH),F5/直接跑都报"找不到 Qt6*.dll";且 Qt Widgets 还要平台插件
# plugins/platforms/qwindows.dll。install 期 windeployqt 只覆盖 cmake --install 的产物,
# 管不到构建树里 VS 直接运行的那个 exe。这里在每次链接完把 Qt 运行依赖(含插件)拷到
# exe 同目录,使 F5 / 资源管理器双击 / ctest 都不依赖 PATH。
#
# 用法:任一层级 include 一次(函数对其所有子目录可见),再对每个 Qt 可执行目标调用:
#   include("${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/qt_windeploy.cmake")
#   qt_windeploy(main)
# 非 Windows / 无 Qt / 缺 windeployqt 时静默跳过,不报错。
function(qt_windeploy TARGET)
  if(NOT WIN32)
    return()
  endif()
  if(NOT TARGET Qt6::Core)
    message(WARNING "qt_windeploy(${TARGET}): 未找到 Qt6::Core,跳过部署")
    return()
  endif()
  get_target_property(_qmake_executable Qt6::qmake IMPORTED_LOCATION)
  if(NOT _qmake_executable)
    message(WARNING "qt_windeploy(${TARGET}): 未找到 Qt6::qmake,跳过部署")
    return()
  endif()
  # qmake 与 windeployqt 同在 Qt 的 bin 目录
  cmake_path(GET _qmake_executable PARENT_PATH _qt_bin_dir)
  find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")
  if(NOT WINDEPLOYQT_EXECUTABLE)
    message(WARNING "qt_windeploy(${TARGET}): 未找到 windeployqt,跳过部署")
    return()
  endif()
  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            --release --no-compiler-runtime --no-translations
            --dir "$<TARGET_FILE_DIR:${TARGET}>"
            "$<TARGET_FILE:${TARGET}>"
    COMMENT "windeployqt -> $<TARGET_FILE_DIR:${TARGET}>"
    VERBATIM)
endfunction()

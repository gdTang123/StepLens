# viewer：ViewerWidget、OcctViewController、InteractionDegrader、ScreenshotService（T09）
# vis（含 TKOpenGl）PRIVATE：不传给链接 viewer 的其它目标。
steplens_add_module(viewer Anchor.cpp)
target_link_libraries(steplens_viewer PRIVATE steplens::occt_vis)

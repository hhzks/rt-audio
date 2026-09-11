corrosion_import_crate(MANIFEST_PATH Cargo.toml CRATES rig_tui LOCKED)
corrosion_link_libraries(rig_tui rt_ffi)

add_executable(rt_rig main.cpp)
target_link_libraries(rt_rig PRIVATE rig_tui rt_ffi)
rt_set_warnings(rt_rig)

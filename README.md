build skia with:

bin/gn gen out/Static --args='skia_enable_ganesh=false skia_use_vulkan=true skia_enable_graphite=true is_official_build=true target_cpu="x64" extra_cflags=["-march=x86-64-v3"]'
ninja -C out/Static

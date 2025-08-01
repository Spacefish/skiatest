build skia with:

bin/gn gen out/Static --args='skia_use_vulkan=true skia_enable_graphite=true'
ninja -C out/Static

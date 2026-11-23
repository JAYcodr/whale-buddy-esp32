# attic

`p0_demo.{cpp,h}`：硬件点屏验证页（fps / heap / psram / 电量 / 触摸 / 时钟同屏），
不参与编译；换板子需要快速验屏时，直接拷回 `src/ui/` 并把 `p0_demo.cpp` 加进 `build_src_filter` 即可。

/* cgo 只会自动编译 package 目录中的 C 文件。
 * Goal 001 暂时通过这个 adapter 引入 dataplane/core 中的实现。
 * Goal 002 前会替换为更稳定的 native C 构建组织。
 */
#include "../../dataplane/core/dp_runtime.c"

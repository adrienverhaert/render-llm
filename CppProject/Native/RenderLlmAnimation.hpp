#pragma once
#include "Common.hpp"
namespace CppProject {
// Select authored property keys, then use the retained easing/interpolation code.
// False leaves legacy/unbound channels on their original evaluation path.
bool RenderLlmEvaluateChannel(IntType timeline, IntType channel);
}

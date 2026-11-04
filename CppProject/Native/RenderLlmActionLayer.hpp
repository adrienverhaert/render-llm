#pragma once

#include "Asset/Scope.hpp"

namespace CppProject
{
	// Returns 1 when a native action succeeded, -1 when it was handled but
	// failed validation/execution, and 0 when the legacy dispatcher should try it.
	IntType renderllm_action_execute(ScopeAny self, VarType id, VarType name, VarType args);
}

#pragma once

#include <QString>

namespace CppProject::StartupTrace
{
	// Opt-in startup diagnostics. Output is fixed below the application Data folder.
	void Mark(const char* stage);
	void Mark(const QString& stage);
}

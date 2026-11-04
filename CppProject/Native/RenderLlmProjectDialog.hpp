#pragma once
#include <QString>

namespace CppProject
{
	void renderllm_open_project_dialog();
	QString renderllm_current_project_name();
	bool renderllm_save_current_project();
	void renderllm_reset_project_file();
	void renderllm_mark_project_clean();
	bool renderllm_project_has_changes();
	bool renderllm_new_project();
	bool renderllm_confirm_project_change();
	void renderllm_new_project_dialog();
}

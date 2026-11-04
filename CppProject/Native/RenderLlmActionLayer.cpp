#include "RenderLlmActionLayer.hpp"

#include "Generated/GmlFunc.hpp"
#include "Generated/Scripts.hpp"

namespace CppProject
{
	namespace
	{
		IntType Fail(VarType id, VarType name)
		{
			mi3_hl_op_fail(id, name, renderllm_document_error());
			return -1;
		}

		IntType Fail(VarType id, VarType name, const StringType& message)
		{
			mi3_hl_op_fail(id, name, message);
			return -1;
		}

		IntType BeginResult(VarType id, VarType name)
		{
			return mi3_hl_op_ok(id, name);
		}
	}

	IntType renderllm_action_execute(ScopeAny self, VarType id, VarType name, VarType args)
	{
		const QString action = name.ToStr().QStr();

		if (action == QStringLiteral("document.canonicalize"))
		{
			const VarType document = mi3_hl_arg_str(args, /*"document"*/ STR(7577), /*""*/ STR(0));
			const VarType canonical = renderllm_document_canonicalize(document);
			if (canonical == /*""*/ STR(0))
				return Fail(id, name);
			const IntType result = BeginResult(id, name);
			DsMap(result)[/*"document"*/ STR(7577)] = canonical;
			return 1;
		}

		if (action == QStringLiteral("document.snapshot"))
		{
			app_update_animate(self);
			tl_update_matrix(self);
			const VarType snapshot = renderllm_document_snapshot();
			if (snapshot == /*""*/ STR(0))
				return Fail(id, name);
			const IntType result = BeginResult(id, name);
			DsMap(result)[/*"document"*/ STR(7577)] = snapshot;
			DsMap(result)[/*"revision"*/ STR(7582)] = renderllm_document_revision();
			return 1;
		}

		if (action == QStringLiteral("document.apply"))
		{
			const IntType currentRevision = renderllm_document_revision();
			const RealType expectedRevision = mi3_hl_arg_real(
				args, /*"expected_revision"*/ STR(7585), currentRevision);
			if (expectedRevision != currentRevision)
			{
				return Fail(id, name,
					StringType(QStringLiteral("Stale document revision: expected %1, current %2.")
						.arg(expectedRevision).arg(currentRevision)));
			}
			const VarType document = mi3_hl_arg_str(args, /*"document"*/ STR(7577), /*""*/ STR(0));
			if (!renderllm_document_apply(document))
				return Fail(id, name);
			const IntType result = BeginResult(id, name);
			DsMap(result)[/*"document"*/ STR(7577)] = renderllm_document_snapshot();
			DsMap(result)[/*"objects"*/ STR(2591)] = ds_list_size(global::_app->project_timeline_list);
			DsMap(result)[/*"revision"*/ STR(7582)] = renderllm_document_revision();
			return 1;
		}

		if (action == QStringLiteral("document.undo") || action == QStringLiteral("document.redo"))
		{
			const bool undo = action == QStringLiteral("document.undo");
			if (!(undo ? renderllm_document_undo() : renderllm_document_redo()))
				return Fail(id, name);
			const IntType result = BeginResult(id, name);
			DsMap(result)[/*"document"*/ STR(7577)] = renderllm_document_snapshot();
			DsMap(result)[/*"objects"*/ STR(2591)] = ds_list_size(global::_app->project_timeline_list);
			DsMap(result)[/*"revision"*/ STR(7582)] = renderllm_document_revision();
			return 1;
		}

		if (action == QStringLiteral("document.open"))
		{
			const VarType documentName = mi3_hl_arg_str(args, /*"name"*/ STR(1015), /*""*/ STR(0));
			if (!renderllm_document_open(documentName))
				return Fail(id, name);
			const IntType result = BeginResult(id, name);
			DsMap(result)[/*"name"*/ STR(1015)] = documentName;
			DsMap(result)[/*"document"*/ STR(7577)] = renderllm_document_snapshot();
			DsMap(result)[/*"revision"*/ STR(7582)] = renderllm_document_revision();
			return 1;
		}

		if (action == QStringLiteral("document.save"))
		{
			const VarType documentName = mi3_hl_arg_str(args, /*"name"*/ STR(1015), /*""*/ STR(0));
			if (!renderllm_document_save(documentName))
				return Fail(id, name);
			const IntType result = BeginResult(id, name);
			DsMap(result)[/*"name"*/ STR(1015)] = documentName;
			DsMap(result)[/*"revision"*/ STR(7582)] = renderllm_document_revision();
			return 1;
		}

		return 0;
	}
}

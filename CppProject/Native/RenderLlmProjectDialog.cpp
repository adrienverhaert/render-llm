#include "RenderLlmProjectDialog.hpp"
#include "StudioUi.hpp"
#include "AppHandler.hpp"
#include "AppWindow.hpp"

#include "Generated/GmlFunc.hpp"
#include "Generated/Scripts.hpp"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>

namespace CppProject
{
	namespace
	{
		class ProjectDialog final : public QDialog
		{
		public:
			explicit ProjectDialog(QWidget* parent) : QDialog(parent)
			{
				setAttribute(Qt::WA_DeleteOnClose);
				setWindowTitle(QStringLiteral("render-llm projects"));
				setFixedWidth(440);
				setStyleSheet(StudioUi::StyleSheet());

				auto* root = new QVBoxLayout(this);
				root->setContentsMargins(28, 28, 28, 28);
				root->setSpacing(12);
				auto* title = new QLabel(QStringLiteral("PROJECTS"));
				title->setProperty("role", "title");
				root->addWidget(title);

				projects = new QListWidget();
				projects->setMinimumHeight(150);
				root->addWidget(projects);
				name = new QLineEdit(QStringLiteral("untitled.renderllm"));
				name->setMaxLength(128);
				name->setPlaceholderText(QStringLiteral("project.renderllm"));
				root->addWidget(name);
				status = new QLabel(QStringLiteral("Projects stay inside Data/renderllm-projects."));
				status->setWordWrap(true);
				root->addWidget(status);

				auto* actions = new QHBoxLayout();
				open = new QPushButton(QStringLiteral("Open"));
				auto* fresh = new QPushButton(QStringLiteral("New scene"));
				actions->addWidget(fresh);
				connect(fresh, &QPushButton::clicked, this, []() { renderllm_new_project_dialog(); });
				save = new QPushButton(QStringLiteral("Save as"));
				actions->addWidget(open);
				actions->addStretch();
				actions->addWidget(save);
				root->addLayout(actions);

				connect(projects, &QListWidget::itemSelectionChanged, this, [this]() {
					if (const QListWidgetItem* selected = projects->currentItem()) name->setText(selected->text());
				});
				connect(projects, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { OpenProject(); });
				connect(open, &QPushButton::clicked, this, [this]() { OpenProject(); });
				connect(save, &QPushButton::clicked, this, [this]() { SaveProject(false); });
				Refresh();
				if (qEnvironmentVariableIsSet("RENDER_LLM_TEST_PROJECT_SAVE"))
					QTimer::singleShot(500, this, [this]() { TestSave(); });

				if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_PROJECT_UI"))
				{
					QTimer::singleShot(700, this, [this]() {
						grab().save(QDir::current().filePath(QStringLiteral("Data/renderllm-project-ui.png")), "PNG");
					});
				}
				const bool runSave = qEnvironmentVariableIsSet("RENDER_LLM_RUN_PROJECT_SAVE");
				const bool runOpen = qEnvironmentVariableIsSet("RENDER_LLM_RUN_PROJECT_OPEN");
				if (runSave || runOpen)
				{
					const QString requested = qEnvironmentVariable("RENDER_LLM_PROJECT_NAME", QStringLiteral("ui-project-smoke.renderllm"));
					name->setText(requested);
					if (runSave) QTimer::singleShot(1100, this, [this]() { SaveProject(true); });
					else QTimer::singleShot(1100, this, [this]() { OpenProject(); });
				}
			}

		private:
			void TestSave()
			{
				const QString first = "savecheck-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".renderllm";
				const QString second = "savecheck-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".renderllm";
				const QString third = "savecheck-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".renderllm";
				const QString firstPath = QDir(StorageRoot()).filePath(first), secondPath = QDir(StorageRoot()).filePath(second);
				auto read = [](const QString& path) {QFile f(path);return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();};
				QJsonArray checks;QJsonObject diagnostics;bool passed = true;
				auto check = [&](const char* label, bool ok) {checks.append(QJsonObject{{"name",label},{"passed",ok}});passed &= ok;};
				const auto original = renderllm_document_snapshot();
				check("save-as establishes current file", renderllm_document_save(StringType(first)) && renderllm_current_project_name() == first);
				auto root = QJsonDocument::fromJson(original.ToUtf8()).object();auto scene = root["scene"].toObject();auto world = scene["world"].toObject();world["color"] = "#123456";scene["world"] = world;root["scene"] = scene;
				check("apply preserves current file", renderllm_document_apply(StringType(QJsonDocument(root).toJson())) && renderllm_current_project_name() == first);
				const auto changed = renderllm_document_snapshot();const auto revision = renderllm_document_revision();
				global::_app->project_changed = true;
				action_toolbar_save(ScopeAny(global::_app->id));
				check("toolbar saves current snapshot without revision", QJsonDocument::fromJson(read(firstPath)) == QJsonDocument::fromJson(changed.ToUtf8()) && renderllm_document_revision() == revision && !renderllm_project_has_changes() && !global::_app->project_changed);
				check("failed open retains current file", !renderllm_document_open(StringType(second)) && renderllm_current_project_name() == first);
				QByteArray external = read(firstPath) + "\n";QSaveFile file(firstPath);const bool wrote = file.open(QIODevice::WriteOnly) && file.write(external) == external.size() && file.commit();
				check("external change is preserved", wrote && !renderllm_save_current_project() && read(firstPath) == external && renderllm_document_snapshot() == changed);
				check("save-as switches target", renderllm_document_save(StringType(second)) && renderllm_current_project_name() == second && renderllm_save_current_project());
				check("failed save preserves target", !renderllm_document_save("../escape.renderllm") && renderllm_current_project_name() == second);
				check("undo remains available after save", renderllm_document_undo() && renderllm_document_snapshot() == original);
				auto choose = [](QString label, int delay = 50) {
					QTimer::singleShot(delay, qApp, [label]() {
						auto* dialog = QApplication::activeModalWidget();
						if (!dialog) dialog = QApplication::activeWindow();
						if (dialog) for (auto* button : dialog->findChildren<QPushButton*>()) if (button->text() == label) {
							dialog->grab().save(QDir::current().filePath("Data/project-lifecycle-prompt.png"));button->click();return;
						}
						qWarning("Native lifecycle test could not find its dialog button");qApp->exit(4);
					});
				};
				check("undo of saved edit is dirty", renderllm_project_has_changes());
				choose("Cancel");check("cancel preserves unsaved scene", !renderllm_confirm_project_change() && renderllm_document_snapshot() == original);
				choose("Discard");check("discard decision does not mutate scene", renderllm_confirm_project_change() && renderllm_project_has_changes() && renderllm_document_snapshot() == original);
				choose("Save");check("save before continuation", renderllm_confirm_project_change() && !renderllm_project_has_changes());
				check("edit becomes dirty", renderllm_document_apply(changed) && renderllm_project_has_changes());
				name->setText(first);choose("Cancel");OpenProject();check("cancel open preserves scene and filename", renderllm_document_snapshot() == changed && renderllm_current_project_name() == second);
				choose("Cancel");action_toolbar_new(ScopeAny(global::_app->id));check("cancel new preserves scene", renderllm_document_snapshot() == changed && renderllm_current_project_name() == second);
				auto* window = App->mainWindow;choose("Cancel");window->closing = true;window->close();
				check("cancel window close leaves editor usable", App->mainWindow == window && !window->closing && renderllm_document_snapshot() == changed);
				QSaveFile outside(secondPath);const auto secondExternal = read(secondPath) + "\n";
				const bool outsideWritten = outside.open(QIODevice::WriteOnly) && outside.write(secondExternal) == secondExternal.size() && outside.commit();
				choose("Save");choose("Close",120);
				check("failed guard save keeps scene and external file", outsideWritten && !renderllm_confirm_project_change() && renderllm_document_snapshot() == changed && read(secondPath) == secondExternal);
				choose("Discard");action_toolbar_new(ScopeAny(global::_app->id));
				diagnostics["new_error"] = renderllm_document_error().QStr();diagnostics["new_name"] = renderllm_current_project_name();diagnostics["new_dirty"] = renderllm_project_has_changes();diagnostics["new_window_state"] = global::_app->window_state.QStr();diagnostics["new_document"] = QJsonDocument::fromJson(renderllm_document_snapshot().ToUtf8()).object();
				check("new is empty unnamed and clean", renderllm_current_project_name().isEmpty() && !renderllm_project_has_changes() && QJsonDocument::fromJson(renderllm_document_snapshot().ToUtf8()).object()["scene"].toObject()["objects"].toArray().isEmpty() && global::_app->window_state == "");
				action_toolbar_open(ScopeAny(global::_app->id));check("open shortcut stays native", global::_app->window_state == "" && isVisible());
				const auto emptyScene = renderllm_document_snapshot();action_toolbar_import_asset(ScopeAny(global::_app->id));
				check("retired asset importer stays inaccessible", global::_app->window_state == "" && renderllm_document_snapshot() == emptyScene);
				const auto dropped = ArrType::From({"retired.miproject","texture.png"});
				check("retired file drops rejected", !window_drop_enter(ScopeAny(global::_app->id), dropped));window_drop(ScopeAny(global::_app->id),dropped);
				check("rejected drop leaves scene intact", global::_app->window_state == "" && renderllm_document_snapshot() == emptyScene);
				renderllm_document_apply(changed);choose("Save");choose("Cancel",120);
				check("cancel unnamed save keeps scene", !renderllm_confirm_project_change() && renderllm_current_project_name().isEmpty() && renderllm_document_snapshot() == changed);
				choose("Save");QTimer::singleShot(120,qApp,[third]() {auto* dialog=QApplication::activeModalWidget();if(!dialog)dialog=QApplication::activeWindow();if(dialog)if(auto* edit=dialog->findChild<QLineEdit*>()){edit->setText(third);return;}qApp->exit(4);});choose("Save scene",180);
				check("first save can continue without losing scene", renderllm_confirm_project_change() && renderllm_current_project_name() == third && !renderllm_project_has_changes() && renderllm_document_snapshot() == changed);
				renderllm_reset_project_file();check("reset clears target", renderllm_current_project_name().isEmpty() && !renderllm_save_current_project());
				// These names were generated here; never delete a caller-supplied project.
				QFile::remove(firstPath);QFile::remove(secondPath);QFile::remove(QDir(StorageRoot()).filePath(third));
				QSaveFile report(QDir::current().filePath("Data/project-save-test.json"));
				if(report.open(QIODevice::WriteOnly)){report.write(QJsonDocument(QJsonObject{{"passed",passed},{"checks",checks},{"diagnostics",diagnostics}}).toJson());report.commit();}
				qApp->exit(passed ? 0 : 2);
			}

			QString StorageRoot() const
			{
				return QDir(gmlGlobal::working_directory.QStr()).filePath(QStringLiteral("Data/renderllm-projects"));
			}

			QString FileName()
			{
				QString value = name->text().trimmed();
				if (!value.endsWith(QStringLiteral(".renderllm"), Qt::CaseInsensitive))
					value += QStringLiteral(".renderllm");
				name->setText(value);
				return value;
			}

			bool IsValidFileName(const QString& value)
			{
				const bool valid = !value.isEmpty() && value.size() <= 128
					&& !value.startsWith(QLatin1Char('.'))
					&& !value.contains(QLatin1Char('/')) && !value.contains(QLatin1Char('\\'))
					&& !value.contains(QLatin1Char(':')) && QFileInfo(value).fileName() == value;
				if (!valid) status->setText(QStringLiteral("Use a local .renderllm filename without path components."));
				return valid;
			}

			void Refresh()
			{
				projects->clear();
				QDir directory(StorageRoot());
				const QFileInfoList files = directory.entryInfoList(
					{ QStringLiteral("*.renderllm") }, QDir::Files | QDir::NoSymLinks, QDir::Name | QDir::IgnoreCase);
				for (const QFileInfo& file : files) projects->addItem(file.fileName());
				open->setEnabled(!files.isEmpty());
			}

			void OpenProject()
			{
				const QString fileName = FileName();
				if (!IsValidFileName(fileName)) return;
				if (!renderllm_confirm_project_change()) return;
				if (!renderllm_document_open(StringType(fileName)))
				{
					status->setText(renderllm_document_error().QStr());
					return;
				}
				status->setText(QStringLiteral("Opened %1 - revision %2")
					.arg(fileName).arg(static_cast<int>(renderllm_document_revision())));
			}

			void SaveProject(bool automated)
			{
				const QString fileName = FileName();
				if (!IsValidFileName(fileName)) return;
				const QFileInfo target(QDir(StorageRoot()).filePath(fileName));
				if (!automated && target.exists()
					&& StudioUi::Message("Replace this project?", QStringLiteral("Replace %1? This cannot be undone.").arg(fileName), {"Replace project", "Keep existing"}, 1) != 0)
					return;
				if (!renderllm_document_save(StringType(fileName)))
				{
					status->setText(renderllm_document_error().QStr());
					return;
				}
				Refresh();
				status->setText(QStringLiteral("Saved %1").arg(fileName));
			}

			QListWidget* projects = nullptr;
			QLineEdit* name = nullptr;
			QLabel* status = nullptr;
			QPushButton* open = nullptr;
			QPushButton* save = nullptr;
		};

		QPointer<ProjectDialog> activeDialog;

		bool SaveBeforeContinue()
		{
			if (!renderllm_current_project_name().isEmpty()) {
				if (renderllm_save_current_project()) return true;
				StudioUi::Message("Could not save",renderllm_document_error().QStr(),{"Close"},0);return false;
			}
			QDialog dialog(QApplication::activeWindow());dialog.setWindowTitle("Save scene · render-llm");dialog.setStyleSheet(StudioUi::StyleSheet());
			auto* layout = new QVBoxLayout(&dialog);layout->setContentsMargins(28,28,28,28);layout->setSpacing(16);
			auto* title = new QLabel("Save your scene");title->setProperty("role","title");layout->addWidget(title);
			auto* name = new QLineEdit("untitled.renderllm");name->setMaxLength(128);layout->addWidget(name);
			auto* status = new QLabel("Choose a name for this project.");status->setWordWrap(true);layout->addWidget(status);
			auto* buttons = new QHBoxLayout;layout->addLayout(buttons);auto* cancel = new QPushButton("Cancel");auto* save = new QPushButton("Save scene");buttons->addWidget(cancel);buttons->addWidget(save);save->setDefault(true);
			QObject::connect(cancel,&QPushButton::clicked,&dialog,&QDialog::reject);
			QObject::connect(save,&QPushButton::clicked,&dialog,[&]() {
				QString file = name->text().trimmed();if(!file.endsWith(".renderllm",Qt::CaseInsensitive))file += ".renderllm";
				// The bridge validates the bounded filename; an existing local file needs an explicit replacement choice.
				if(QFileInfo(file).fileName()==file && QFileInfo(QDir(gmlGlobal::working_directory.QStr()).filePath("Data/renderllm-projects/"+file)).exists()
					&& StudioUi::Message("Replace this project?",file,{"Replace project","Keep existing"},1)!=0)return;
				if(!renderllm_document_save(StringType(file))){status->setText(renderllm_document_error().QStr());return;}
				dialog.accept();
			});
			dialog.resize(440,220);return App->ExecDialog(&dialog) == QDialog::Accepted;
		}
	}

	bool renderllm_confirm_project_change()
	{
		if (!renderllm_project_has_changes()) return true;
		const int choice = StudioUi::Message("Keep your changes?","This scene has changes that have not been saved.",{"Save","Discard","Cancel"},2);
		return choice == 1 || (choice == 0 && SaveBeforeContinue());
	}
	void renderllm_new_project_dialog()
	{
		if (!renderllm_confirm_project_change()) return;
		if (!renderllm_new_project()) StudioUi::Message("Could not create scene",renderllm_document_error().QStr(),{"Close"},0);
	}

	void renderllm_open_project_dialog()
	{
		if (activeDialog)
		{
			activeDialog->raise();
			activeDialog->activateWindow();
			return;
		}
		activeDialog = new ProjectDialog(QApplication::activeWindow());
		activeDialog->show();
	}
}

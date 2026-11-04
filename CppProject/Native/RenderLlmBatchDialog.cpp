#include "RenderLlmBatchDialog.hpp"
#include "StudioUi.hpp"
#include "StudioWidgets.hpp"
#include "Generated/Scripts.hpp"
#include "AppHandler.hpp"
#include "AppWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QSaveFile>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace CppProject
{
	namespace
	{
		class BatchDialog final : public QDialog
		{
		public:
			explicit BatchDialog(QWidget* parent) : QDialog(parent)
			{
				setAttribute(Qt::WA_DeleteOnClose);
				setWindowTitle(QStringLiteral("Color variants · render-llm"));
				resize(760,560);
				setStyleSheet(StudioUi::StyleSheet());

				auto* root = new QVBoxLayout(this);
				root->setContentsMargins(28, 28, 28, 28);
				root->setSpacing(14);
				auto* title = new QLabel(QStringLiteral("One object. Four colors."));
				title->setProperty("role", "title");
				root->addWidget(title);

				auto* form = new QFormLayout();
				objectId = new QComboBox;
				const auto document=QJsonDocument::fromJson(renderllm_document_snapshot().ToUtf8()).object();
				const auto objects=document.value("scene").toObject().value("objects").toArray();
				for(const auto& entry:objects) {
					const auto object=entry.toObject();
					if(!QStringList{"cube","sphere","cylinder","cone","plane"}.contains(object.value("kind").toString()))continue;
					const auto material=object.value("material_id").toString();if(material.isEmpty()||!document.value("materials").toObject().contains(material))continue;
					int users=0;for(const auto& candidate:objects)if(candidate.toObject().value("material_id")==material)++users;
					bool keyed=false;for(const auto& track:document.value("animation").toObject().value("tracks").toArray())if(track.toObject().value("target")==object.value("id")&&track.toObject().value("property")=="material.color")keyed=true;
					if(users!=1||keyed)continue;
					objectId->addItem(object.value("name").toString(object.value("id").toString()),object.value("id").toString());
				}
				objectId->setToolTip("Objects with an independent material and no animated color. Shared or keyed colors are excluded to protect the scene.");
				if(qEnvironmentVariableIsSet("RENDER_LLM_RUN_BATCH_UI"))objectId->setCurrentIndex(qMax(0,objectId->findData("sphere")));
				form->addRow(QStringLiteral("Object"), objectId);
				resolution = new QComboBox();
				resolution->addItems({ QStringLiteral("384 x 216"), QStringLiteral("768 x 432"), QStringLiteral("1280 x 720") });
				form->addRow(QStringLiteral("Output"), resolution);
				samples = new QComboBox();
				samples->addItems({ QStringLiteral("1"), QStringLiteral("4"), QStringLiteral("16"), QStringLiteral("64") });
				samples->setCurrentText(QStringLiteral("4"));
				form->addRow(QStringLiteral("Samples"), samples);
				denoise = new StudioUi::ToggleBox(QStringLiteral("Denoise"));
				denoise->setChecked(true);
				form->addRow(QString(), denoise);
				root->addLayout(form);

				auto* colors = new QGridLayout();
				const QStringList defaults = { QStringLiteral("#FF0000"), QStringLiteral("#0066FF"), QStringLiteral("#FFD700"), QStringLiteral("#00CC88") };
				for (int index = 0; index < defaults.size(); ++index)
				{
					auto tile=new QWidget;auto tileLayout=new QVBoxLayout(tile);tileLayout->setSpacing(10);
					auto swatch=new QPushButton;swatch->setMinimumHeight(64);swatch->setAccessibleName(QString("Variant %1 color").arg(index+1));
					tileLayout->addWidget(swatch);
					colorEdits[index] = new QLineEdit(defaults[index]);
					colorEdits[index]->setMaxLength(7);
					tileLayout->addWidget(colorEdits[index]);
					connect(colorEdits[index],&QLineEdit::textChanged,tile,[=](const QString& text){if(QRegularExpression("^#[0-9A-Fa-f]{6}$").match(text).hasMatch())swatch->setStyleSheet(QString("QPushButton{background:%1;border:0;border-radius:8px;}").arg(text));});
					colorEdits[index]->textChanged(defaults[index]);
					connect(swatch,&QPushButton::clicked,this,[=](){QColor c(colorEdits[index]->text());StudioUi::PickColor(c.red()|(c.green()<<8)|(c.blue()<<16),[guard=QPointer<QLineEdit>(colorEdits[index])](IntType rgb){if(guard)guard->setText(QColor(rgb&255,(rgb>>8)&255,(rgb>>16)&255).name().toUpper());});});
					colorTiles[index]=tile;colors->addWidget(tile,0,index);
				}
				root->addLayout(colors);

				status = new QLabel(objectId->count()?QStringLiteral("Only this object's color changes. Camera, lighting and every other object stay identical."):QStringLiteral("No eligible object. Use a shape with its own material and no animated color, then reopen Variants."));
				status->setWordWrap(true);
				root->addWidget(status);
				auto* actions = new QHBoxLayout();
				openOutputs = new QPushButton(QStringLiteral("Open outputs"));
				openOutputs->setEnabled(false);
				generate = new QPushButton(QStringLiteral("Generate 4"));
				generate->setEnabled(objectId->count()>0);
				cancel=new QPushButton("Cancel");cancel->hide();actions->addWidget(cancel);
				connect(cancel,&QPushButton::clicked,this,[this](){Cancel(false);});
				actions->addWidget(openOutputs);
				actions->addStretch();
				actions->addWidget(generate);
				root->addLayout(actions);

				connect(generate, &QPushButton::clicked, this, [this]() { StartBatch(); });
				connect(openOutputs, &QPushButton::clicked, this, [this]() {
					if (!outputDirectory.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(outputDirectory));
				});

				if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_BATCH_UI"))
				{
					QTimer::singleShot(700, this, [this]() {
						grab().save(QDir::current().filePath(QStringLiteral("Data/renderllm-batch-ui.png")), "PNG");
					});
				}
				if (qEnvironmentVariableIsSet("RENDER_LLM_RUN_BATCH_UI"))
					QTimer::singleShot(1200, this, [this]() { StartBatch(); });
			}
			bool StopForExit(){if(!process)return false;Cancel(false);raise();activateWindow();return true;}
		protected:
			void reject() override {if(process)Cancel(true);else QDialog::reject();}
			void closeEvent(QCloseEvent* event) override {if(process){Cancel(true);event->ignore();}else QDialog::closeEvent(event);}

		private:
			void Cancel(bool closeWhenDone){if(!process)return;closeRequested|=closeWhenDone;if(cancelRequested)return;cancelRequested=true;cancel->setEnabled(false);status->setText("Cancelling safely…");if(process->state()==QProcess::Running)process->write("cancel\n");}
			void SetRunning(bool running){generate->setEnabled(!running&&objectId->count()>0);cancel->setVisible(running);cancel->setEnabled(running);objectId->setEnabled(!running);resolution->setEnabled(!running);samples->setEnabled(!running);denoise->setEnabled(!running);for(auto tile:colorTiles)tile->setEnabled(!running);}
			void Finish(int exitCode,QProcess::ExitStatus exitStatus){
				if(!process)return;
				const auto bytes=process->readAllStandardOutput().trimmed();const auto errors=process->readAllStandardError().trimmed();
				process->deleteLater();process=nullptr;SetRunning(false);
				const auto result=QJsonDocument::fromJson(bytes).object();const bool cancelled=result.value("cancelled").toBool();
				bool valid=exitCode==0&&exitStatus==QProcess::NormalExit;
				if(!valid)status->setText(QString::fromUtf8(errors.isEmpty()?QByteArray("Batch could not finish. Your scene is unchanged."):errors.right(1600)));
				else if(cancelled)status->setText("Cancelled · your scene is unchanged");
				else {
					const auto outputs=result.value("outputs").toArray();valid=outputs.size()==4;
					const auto dataDir=QDir::current().filePath("Data/assistant");
					outputDirectory=QDir::cleanPath(QDir(dataDir).filePath(result.value("job_dir").toString()));
					valid=valid&&!result.value("job_dir").toString().isEmpty()&&outputDirectory.startsWith(QDir::cleanPath(dataDir)+"/");
					for(const auto& entry:outputs){const auto file=QDir::cleanPath(QDir(dataDir).filePath(entry.toObject().value("image").toString()));valid=valid&&file.startsWith(outputDirectory+"/")&&QFileInfo(file).isFile()&&QFileInfo(file).size()>0&&!QPixmap(file).isNull();}
					const auto diagnostic=outputs.isEmpty()?QJsonObject():outputs.first().toObject();
					openOutputs->setEnabled(valid);status->setText(valid?QString("Four variants ready · %1 / %2\nOriginal scene unchanged").arg(diagnostic.value("backend").toString(),diagnostic.value("denoiser").toString()):"Batch returned invalid or unreadable outputs.");
				}
				if(qEnvironmentVariableIsSet("RENDER_LLM_RUN_BATCH_UI")) {
					const bool unchanged=renderllm_document_snapshot().QStr()==beforeDocument;
					const QString mode=qEnvironmentVariable("RENDER_LLM_TEST_CANCEL_BATCH");
					const bool passed=valid&&unchanged&&(mode.isEmpty()?!cancelled:cancelled&&visibleWhileCancelling);
					QSaveFile report(QDir::current().filePath("Data/studio-batch-lifecycle.json"));if(report.open(QIODevice::WriteOnly)){report.write(QJsonDocument(QJsonObject{{"passed",passed},{"mode",mode},{"scene_unchanged",unchanged},{"cancelled",cancelled},{"result",result}}).toJson());report.commit();}
					grab().save(QDir::current().filePath("Data/studio-batch-result.png"));QTimer::singleShot(200,qApp,[passed](){qApp->exit(passed?0:2);});
				}
				if(closeRequested)QDialog::reject();
			}
			void StartBatch()
			{
				if(process)return;
				outputDirectory.clear();
				const QRegularExpression colorPattern(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
				QStringList colors;
				for (QLineEdit* edit : colorEdits)
				{
					const QString color = edit->text().trimmed();
					if (!colorPattern.match(color).hasMatch())
					{
						status->setText(QStringLiteral("Each color must use #RRGGBB."));
						return;
					}
					colors.append(color.toUpper());
				}
				const QString id = objectId->currentData().toString();
				if (id.isEmpty() || id.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1F]"))))
				{
					status->setText(QStringLiteral("Enter a valid Shape ID."));
					return;
				}

				const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
				const QString script = QDir::current().filePath(QStringLiteral("ASSISTANT/dist/batch-ui.js"));
				const QString dataDir = QDir::current().filePath(QStringLiteral("Data/assistant"));
				if (node.isEmpty() || !QFileInfo::exists(script))
				{
					status->setText(QStringLiteral("The packaged Batch runtime is unavailable."));
					return;
				}
				const QStringList dimensions = resolution->currentText().split(QLatin1Char(' '), Qt::SkipEmptyParts);
				QStringList arguments = {
					script, QStringLiteral("--dir"), dataDir,
					QStringLiteral("--object"), id,
					QStringLiteral("--colors"), colors.join(QLatin1Char(',')),
					QStringLiteral("--width"), dimensions.value(0),
					QStringLiteral("--height"), dimensions.value(2),
					QStringLiteral("--samples"), samples->currentText(),
					QStringLiteral("--denoise"), denoise->isChecked() ? QStringLiteral("true") : QStringLiteral("false")
				};
				process = new QProcess(this);
				beforeDocument=renderllm_document_snapshot().QStr();cancelRequested=false;closeRequested=false;visibleWhileCancelling=false;SetRunning(true);
				openOutputs->setEnabled(false);
				status->setText(QStringLiteral("Rendering four isolated variants…"));
				connect(process,&QProcess::started,this,[this](){if(cancelRequested)process->write("cancel\n");});
				connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
					[this](int exitCode, QProcess::ExitStatus exitStatus) {Finish(exitCode,exitStatus);});
				connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
					if (error != QProcess::FailedToStart) return;
					Finish(-1,QProcess::CrashExit);
				});
				process->start(node, arguments);
				if(qEnvironmentVariableIsSet("RENDER_LLM_TEST_CANCEL_BATCH"))QTimer::singleShot(1600,this,[this](){
					if(!process)return;const auto mode=qEnvironmentVariable("RENDER_LLM_TEST_CANCEL_BATCH");
					if(mode=="escape"){QKeyEvent key(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(this,&key);}
					else if(mode=="close")close();else if(mode=="editor-close"){if(App&&App->mainWindow){App->mainWindow->closing=true;App->mainWindow->close();}}else cancel->click();
					visibleWhileCancelling=isVisible()&&process!=nullptr&&App&&App->mainWindow&&!App->mainWindow->closing;
				});
			}

			QComboBox* objectId = nullptr;
			QWidget* colorTiles[4]={};
			QLineEdit* colorEdits[4] = {};
			QComboBox* resolution = nullptr;
			QComboBox* samples = nullptr;
			QCheckBox* denoise = nullptr;
			QLabel* status = nullptr;
			QPushButton* generate = nullptr;
			QPushButton* openOutputs = nullptr;
			QPushButton* cancel = nullptr;
			QProcess* process = nullptr;
			QString outputDirectory;
			QString beforeDocument;
			bool cancelRequested=false,closeRequested=false,visibleWhileCancelling=false;
		};

		QPointer<BatchDialog> activeDialog;
	}
	bool renderllm_stop_batch_before_exit(){return activeDialog&&activeDialog->StopForExit();}

	void renderllm_open_batch_dialog()
	{
		if (activeDialog)
		{
			activeDialog->raise();
			activeDialog->activateWindow();
			return;
		}
		activeDialog = new BatchDialog(QApplication::activeWindow());
		activeDialog->show();
	}
}

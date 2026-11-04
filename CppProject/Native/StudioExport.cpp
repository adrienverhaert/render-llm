#include "StudioExport.hpp"
#include "StudioUi.hpp"
#include "Generated/Scripts.hpp"
#include "AppHandler.hpp"
#include "AppWindow.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QFileInfo>
#include <QSaveFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QWheelEvent>
#include <cmath>

namespace CppProject::StudioUi {
namespace {
// Scrolling the settings column must not change values under the pointer.
template<class Widget> class ScrollInput final : public Widget {
public:
 ScrollInput(){this->setFocusPolicy(Qt::StrongFocus);}
protected:
 void wheelEvent(QWheelEvent* event) override {if(!this->hasFocus()){event->ignore();return;}Widget::wheelEvent(event);}
};
class ExportDialog final : public QDialog {
public:
 explicit ExportDialog(bool video, QWidget* parent):QDialog(parent) {
  setAttribute(Qt::WA_DeleteOnClose);setWindowTitle("Output · render-llm");resize(940,640);
  const auto document=QJsonDocument::fromJson(renderllm_document_snapshot().ToUtf8()).object();
  const auto animation=document.value("animation").toObject();
  auto root=new QHBoxLayout(this);root->setContentsMargins(28,28,28,28);root->setSpacing(28);
  auto left=new QVBoxLayout;left->setSpacing(16);root->addLayout(left,2);
  auto body=new QWidget;body->setObjectName("output-settings-body");
  auto controls=new QVBoxLayout(body);controls->setContentsMargins(0,0,0,0);controls->setSpacing(16);
  settings=new QScrollArea;settings->setMinimumWidth(320);settings->setFrameShape(QFrame::NoFrame);settings->setWidgetResizable(true);settings->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  settings->setStyleSheet("QScrollArea{background:#171719;border:0;}QWidget#output-settings-body{background:#171719;}");settings->setWidget(body);left->addWidget(settings,1);
  auto title=new QLabel("Create an output");title->setProperty("role","title");controls->addWidget(title);
  kind=new ScrollInput<QComboBox>;kind->addItem("Still image · PNG","image");kind->addItem("Video · MP4","video");kind->addItem("Frame sequence · PNG","sequence");kind->setCurrentIndex(video?1:0);
  if(qEnvironmentVariable("RENDER_LLM_TEST_EXPORT_FORMAT")=="png-sequence")kind->setCurrentIndex(2);
  controls->addWidget(kind);
  auto form=new QFormLayout;form->setVerticalSpacing(16);controls->addLayout(form);
  resolution=new ScrollInput<QComboBox>;resolution->addItems({"Preview · 384 × 216","HD · 1280 × 720","Full HD · 1920 × 1080","Custom"});resolution->setCurrentIndex(1);form->addRow("Size",resolution);
  auto dimensions=new QHBoxLayout;width=new ScrollInput<QSpinBox>;height=new ScrollInput<QSpinBox>;
  width->setRange(320,1920);height->setRange(180,1080);width->setValue(1280);height->setValue(720);
  width->setAccessibleName("Output width in pixels");height->setAccessibleName("Output height in pixels");
  dimensions->addWidget(width);dimensions->addWidget(new QLabel("×"));dimensions->addWidget(height);form->addRow("Pixels",dimensions);
  auto preset=[this](int index){if(index>=3)return;const int widths[]={384,1280,1920},heights[]={216,720,1080};QSignalBlocker w(width),h(height);width->setValue(widths[index]);height->setValue(heights[index]);};
  connect(resolution,qOverload<int>(&QComboBox::currentIndexChanged),this,preset);
  auto custom=[this](){QSignalBlocker block(resolution);resolution->setCurrentIndex(3);};
  connect(width,qOverload<int>(&QSpinBox::valueChanged),this,custom);connect(height,qOverload<int>(&QSpinBox::valueChanged),this,custom);
  samples=new ScrollInput<QSpinBox>;samples->setRange(1,256);samples->setValue(qBound(1,document.value("render").toObject().value("samples").toInt(16),256));form->addRow("Samples",samples);
  frames=new ScrollInput<QSpinBox>;frames->setRange(1,240);form->addRow("Frames",frames);
  fps=new ScrollInput<QSpinBox>;fps->setRange(1,60);fps->setValue(qBound(1,animation.value("fps").toInt(24),60));form->addRow("Frames / second",fps);
  const double sourceFps=animation.value("fps").toDouble(24);
  frames->setValue(qBound(1,int(std::floor((animation.value("frame_count").toInt(1)-1)*fps->value()/sourceFps+1e-6))+1,240));
  auto timing=[=](){const bool movie=kind->currentIndex()!=0;frames->setVisible(movie);fps->setVisible(movie);form->labelForField(frames)->setVisible(movie);form->labelForField(fps)->setVisible(movie);frames->setEnabled(movie);fps->setEnabled(movie);};
  connect(kind,qOverload<int>(&QComboBox::currentIndexChanged),this,timing);timing();
  summary=new QLabel;summary->setWordWrap(true);controls->addWidget(summary);
  auto note=new QLabel("From frame 0 · frozen scene\nUp to 1920 × 1080, 256 samples, 240 frames.\nNoise cleanup follows Look settings.");note->setWordWrap(true);controls->addWidget(note);controls->addStretch();
  start=new QPushButton("Render output");start->setDefault(true);left->addWidget(start);
  auto result=new QVBoxLayout;result->setSpacing(16);root->addLayout(result,3);
  preview=new QLabel("Your output appears here");preview->setAlignment(Qt::AlignCenter);preview->setMinimumSize(280,180);preview->setStyleSheet("background:#101011;border-radius:12px;color:#88867f;");result->addWidget(preview,1);
  status=new QLabel("Ready when you are");status->setWordWrap(true);result->addWidget(status);
  progress=new QProgressBar;progress->setRange(0,0);progress->hide();result->addWidget(progress);
  auto actions=new QHBoxLayout;cancel=new QPushButton("Cancel render");cancel->hide();actions->addWidget(cancel);actions->addStretch();open=new QPushButton("Open output");open->setEnabled(false);actions->addWidget(open);result->addLayout(actions);
  connect(start,&QPushButton::clicked,this,[this](){Start();});
  connect(cancel,&QPushButton::clicked,this,[this](){Cancel(false);});
  connect(open,&QPushButton::clicked,this,[this](){QDesktopServices::openUrl(QUrl::fromLocalFile(output));});
  auto refresh=[this](){if(process)return;summary->setText(kind->currentIndex()==0?QString("%1 × %2 · %3 samples").arg(width->value()).arg(height->value()).arg(samples->value()):QString("%1 frames · %2 seconds at %3 fps").arg(frames->value()).arg(double(frames->value())/fps->value(),0,'f',2).arg(fps->value()));const QString issue=Validation();status->setText(issue.isEmpty()?"Ready when you are":issue);};
  for(auto field:{width,height,samples,frames,fps})connect(field,qOverload<int>(&QSpinBox::valueChanged),this,refresh);
  connect(resolution,qOverload<int>(&QComboBox::currentIndexChanged),this,refresh);connect(kind,qOverload<int>(&QComboBox::currentIndexChanged),this,refresh);
  const auto request=QJsonDocument::fromJson(qEnvironmentVariable("RENDER_LLM_TEST_EXPORT_REQUEST").toUtf8()).object();
  for(auto item:{std::pair<const char*,QSpinBox*>{"width",width},{"height",height},{"samples",samples},{"frames",frames},{"fps",fps}})if(request.contains(item.first))item.second->setValue(request.value(item.first).toInt());
  refresh();
  if(qEnvironmentVariableIsSet("RENDER_LLM_TEST_EXPORT_COMPACT"))resize(760,480);
  if(qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_RENDER_UI")||qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_VIDEO_UI"))QTimer::singleShot(700,this,[this](){grab().save(QDir::current().filePath("Data/studio-output-ui.png"));});
  if(qEnvironmentVariableIsSet("RENDER_LLM_TEST_EXPORT_SETTINGS"))QTimer::singleShot(800,this,[this](){TestSettings();});
  else if(qEnvironmentVariableIsSet("RENDER_LLM_RUN_EXPORT_UI"))QTimer::singleShot(600,this,[this](){Start();});
 }
 bool StopForEditorExit() {
  if(!process)return false;
  editorCloseRequested=true;Cancel(false);raise();activateWindow();return true;
 }
protected:
 void reject() override {if(process)Cancel(true);else QDialog::reject();}
 void closeEvent(QCloseEvent* e) override {if(process){Cancel(true);e->ignore();}else QDialog::closeEvent(e);}
private:
 QString Validation() const {
  for(auto field:{width,height,samples,frames,fps})if(!field->hasAcceptableInput())return "Enter a valid whole number in each output field.";
  const bool movie=kind->currentIndex()!=0;
  if(movie&&(width->value()%2||height->value()%2))return "Animation needs even pixel dimensions. Adjust width or height.";
  const qint64 work=qint64(width->value())*height->value()*samples->value()*(movie?frames->value():1);
  if(work>(movie?8000000000LL:130000000LL))return "This output exceeds the render limit. Reduce pixels, samples or frames.";
  if(movie){
   const auto animation=QJsonDocument::fromJson(renderllm_document_snapshot().ToUtf8()).object().value("animation").toObject();
   const double last=(frames->value()-1)*animation.value("fps").toDouble(24)/fps->value();
   if(last>animation.value("frame_count").toInt(1)-1+1e-6)return "This output extends past the timeline. Reduce frames or edit Motion.";
  }
  return {};
 }
 void TestSettings() {
  const auto before=renderllm_document_snapshot();const auto document=QJsonDocument::fromJson(before.ToUtf8()).object();
  const QJsonObject defaults{{"width",width->value()},{"height",height->value()},{"frames",frames->value()},{"fps",fps->value()},{"samples",samples->value()}};
  QJsonArray checks;bool passed=true;
  auto check=[&](const QString& name,bool value){checks.append(QJsonObject{{"name",name},{"passed",value}});passed=passed&&value;};
  const auto animation=document.value("animation").toObject();
  const int expectedFps=qBound(1,animation.value("fps").toInt(),60);
  const int expectedFrames=qBound(1,int(std::floor((animation.value("frame_count").toInt()-1)*double(expectedFps)/animation.value("fps").toDouble()+1e-6))+1,240);
  check("timeline and samples defaults",frames->value()==expectedFrames&&fps->value()==expectedFps&&samples->value()==qBound(1,document.value("render").toObject().value("samples").toInt(),256));
  check("HD default",width->value()==1280&&height->value()==720&&resolution->currentIndex()==1);
  resolution->setCurrentIndex(0);check("preset dimensions",width->value()==384&&height->value()==216);
  width->setValue(640);height->setValue(360);samples->setValue(13);check("custom dimensions and samples",resolution->currentIndex()==3&&Validation().isEmpty());
  width->setValue(641);Start();check("odd animation rejected before process",!process&&output.isEmpty()&&status->text().contains("even"));width->setValue(640);
  const int initialFrames=frames->value();frames->setValue(240);fps->setValue(1);Start();check("timeline overrun rejected before process",!process&&status->text().contains("timeline"));
  resolution->setCurrentIndex(2);samples->setValue(256);Start();check("animation budget rejected before process",!process&&status->text().contains("render limit"));
  kind->setCurrentIndex(0);Start();check("still budget rejected before process",!process&&status->text().contains("render limit")&&!frames->isVisible()&&!fps->isVisible());
  width->setValue(641);height->setValue(361);samples->setValue(13);check("odd still allowed",Validation().isEmpty());
  kind->setCurrentIndex(2);width->setValue(640);height->setValue(360);fps->setValue(document.value("animation").toObject().value("fps").toInt());frames->setValue(initialFrames);
  check("sequence shares valid timing",Validation().isEmpty()&&frames->isVisible()&&fps->isVisible());
  resize(760,480);QApplication::processEvents();const auto compact=size();
  auto inside=[this](QWidget* widget){return widget->isVisible()&&rect().contains(widget->mapTo(this,QPoint(0,0)))&&rect().contains(widget->mapTo(this,widget->rect().bottomRight()));};
  check("compact size and idle actions",compact==QSize(760,480)&&inside(start)&&inside(open));
  activateWindow();QApplication::processEvents();frames->setFocus();QKeyEvent tab(QEvent::KeyPress,Qt::Key_Tab,Qt::NoModifier);QApplication::sendEvent(frames,&tab);QApplication::processEvents();
  const auto fieldTop=fps->mapTo(settings->viewport(),QPoint(0,0));const auto fieldBottom=fps->mapTo(settings->viewport(),fps->rect().bottomRight());
  check("keyboard reaches scrolled timing field",fps->hasFocus()&&settings->viewport()->rect().contains(fieldTop)&&settings->viewport()->rect().contains(fieldBottom));
  const QJsonObject keyboard{{"fps_focus",fps->hasFocus()},{"viewport_width",settings->viewport()->width()},{"field_left",fieldTop.x()},{"field_right",fieldBottom.x()},{"field_top",fieldTop.y()},{"field_bottom",fieldBottom.y()},{"viewport_height",settings->viewport()->height()}};
  auto wheel=[](QWidget* widget){const QPoint pos=widget->rect().center();QWheelEvent event(pos,widget->mapToGlobal(pos),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(widget,&event);};
  const int sampleBefore=samples->value(),kindBefore=kind->currentIndex();wheel(samples);wheel(kind);
  check("wheel does not change unfocused fields",samples->value()==sampleBefore&&kind->currentIndex()==kindBefore);
  samples->setFocus();wheel(samples);check("focused wheel edits samples",samples->value()==sampleBefore+1);samples->setValue(sampleBefore);
  SetRunning(true);QApplication::processEvents();check("compact running actions",size()==QSize(760,480)&&inside(cancel)&&inside(open));
  check("fields frozen during export",!width->isEnabled()&&!height->isEnabled()&&!samples->isEnabled()&&!frames->isEnabled()&&!fps->isEnabled());SetRunning(false);
  grab().save(QDir::current().filePath("Data/studio-output-compact.png"));
  check("scene unchanged",renderllm_document_snapshot()==before);
  QSaveFile report(QDir::current().filePath("Data/studio-export-settings.json"));if(report.open(QIODevice::WriteOnly)){report.write(QJsonDocument(QJsonObject{{"passed",passed},{"defaults",defaults},{"compact_width",compact.width()},{"compact_height",compact.height()},{"keyboard",keyboard},{"checks",checks}}).toJson());report.commit();}
  grab().save(QDir::current().filePath("Data/studio-export-settings.png"));QTimer::singleShot(200,qApp,[passed](){qApp->exit(passed?0:2);});
 }
 void Cancel(bool closeWhenDone) {
  if(!process)return;
  closeRequested=closeRequested||closeWhenDone;
  if(cancelRequested)return;
  cancelRequested=true;status->setText("Cancelling safely…");cancel->setEnabled(false);
  // A request made during QProcess::Starting is sent by the started handler.
  if(process->state()==QProcess::Running)process->write("cancel\n");
 }
 void Finish(int code,QProcess::ExitStatus state) {
  if(!process)return;
  if(code!=0||state!=QProcess::NormalExit){
   output.clear();open->setEnabled(false);
   status->setText(errors.trimmed().isEmpty()?"Export could not finish. The scene is unchanged.":QString::fromUtf8(errors.right(1600)).trimmed());
   QSaveFile log(QDir(dataDir).filePath("export-ui-last-error.txt"));if(log.open(QIODevice::WriteOnly)){log.write(errors);log.commit();}
  } else if(output.isEmpty()&&!cancelled)status->setText("Export ended without a verified output.");
  if(editorCloseRequested&&cancelled)status->setText("Render cancelled. You can now close the editor.");
  process->deleteLater();process=nullptr;SetRunning(false);
  const QString test=qEnvironmentVariable("RENDER_LLM_TEST_CANCEL_EXPORT");
  if(!test.isEmpty()) {
   const bool failedStart=test=="failed-start";
   const bool passed=failedStart?code!=0&&start->isEnabled():testDispatched&&visibleWhileCancelling&&cancelled&&code==0&&(test!="partial"||partialFrames.size()>=2);
   QSaveFile report(QDir::current().filePath("Data/studio-export-lifecycle.json"));
   if(report.open(QIODevice::WriteOnly)){report.write(QJsonDocument(QJsonObject{{"mode",test},{"passed",passed},{"cancelled",cancelled},{"host_exit",code},{"dialog_alive_until_drained",visibleWhileCancelling},{"close_requested",closeRequested},{"editor_close_requested",editorCloseRequested},{"completed_frames",QJsonArray::fromStringList(partialFrames)},{"scene_unchanged",renderllm_document_snapshot().QStr()==beforeDocument}}).toJson());report.commit();}
   grab().save(QDir::current().filePath("Data/studio-output-result.png"));
   const bool unchanged=renderllm_document_snapshot().QStr()==beforeDocument;
   QTimer::singleShot(200,qApp,[passed,unchanged](){qApp->exit(passed&&unchanged?0:2);});
  } else if(qEnvironmentVariableIsSet("RENDER_LLM_RUN_EXPORT_UI")) {
   const bool unchanged=renderllm_document_snapshot().QStr()==beforeDocument;
   const bool passed=code==0&&state==QProcess::NormalExit&&!output.isEmpty()&&unchanged;
   QSaveFile report(QDir::current().filePath("Data/studio-export-complete.json"));
   if(report.open(QIODevice::WriteOnly)){report.write(QJsonDocument(QJsonObject{{"passed",passed},{"scene_unchanged",unchanged},{"result",finalResult}}).toJson());report.commit();}
   grab().save(QDir::current().filePath("Data/studio-output-result.png"));QTimer::singleShot(200,qApp,[passed](){qApp->exit(passed?0:2);});
  }
  if(closeRequested)QDialog::reject();
 }
 void TestCancel() {
  if(testDispatched||!process)return;
  const QString mode=qEnvironmentVariable("RENDER_LLM_TEST_CANCEL_EXPORT");
  if(mode=="partial") {
   partialFrames.clear();QDirIterator frames(QDir(dataDir).filePath("video"),{"frame-*.png"},QDir::Files,QDirIterator::Subdirectories);
   while(frames.hasNext()){const auto file=frames.next();if(!file.endsWith(".partial.png")&&frames.fileInfo().lastModified()>=testStarted)partialFrames.append(file);}
   if(partialFrames.size()<2&&++partialPolls<150){QTimer::singleShot(200,this,[this](){TestCancel();});return;}
  }
  testDispatched=true;
  if(mode=="escape"||mode=="early") {QKeyEvent key(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(this,&key);}
  else if(mode=="close")close();
  else if(mode=="editor-close") {if(App&&App->mainWindow){App->mainWindow->closing=true;App->mainWindow->close();}else return;}
  else cancel->click();
  visibleWhileCancelling=isVisible()&&process!=nullptr&&App&&App->mainWindow&&!App->mainWindow->closing;
 }
 void Start() {
  if(process)return;
  const QString issue=Validation();if(!issue.isEmpty()){status->setText(issue);return;}
  const QString node=QStandardPaths::findExecutable("node");
  const QString script=QDir::current().filePath("ASSISTANT/dist/export-ui.js");
  if(node.isEmpty()||!QFileInfo::exists(script)){status->setText("Export runtime missing. Rebuild the native package.");return;}
  dataDir=QDir::current().filePath("Data/assistant");
  QStringList args={script,"--dir",dataDir,"--kind",kind->currentData().toString(),"--width",QString::number(width->value()),"--height",QString::number(height->value()),"--samples",QString::number(samples->value()),"--frames",QString::number(frames->value()),"--fps",QString::number(fps->value())};
  beforeDocument=renderllm_document_snapshot().QStr();
  testStarted=QDateTime::currentDateTime();partialPolls=0;partialFrames.clear();
  process=new QProcess(this);buffer.clear();errors.clear();output.clear();cancelled=false;cancelRequested=false;closeRequested=false;editorCloseRequested=false;testDispatched=false;visibleWhileCancelling=false;SetRunning(true);open->setEnabled(false);status->setText("Reading the current scene…");
  preview->clear();preview->setText("Preparing output…");
  connect(process,&QProcess::started,this,[this](){if(cancelRequested)process->write("cancel\n");});
  connect(process,&QProcess::readyReadStandardOutput,this,[this](){buffer+=process->readAllStandardOutput();int i;while((i=buffer.indexOf('\n'))>=0){Read(buffer.left(i));buffer.remove(0,i+1);}});
  connect(process,&QProcess::readyReadStandardError,this,[this](){errors+=process->readAllStandardError();});
  connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus state){Finish(code,state);});
  connect(process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError e){if(e==QProcess::FailedToStart){errors="Could not start the export runtime.";Finish(-1,QProcess::CrashExit);}});
  process->start(qEnvironmentVariable("RENDER_LLM_TEST_CANCEL_EXPORT")=="failed-start"?QDir::current().filePath("missing-export-test-runtime.exe"):node,args);
  if(qEnvironmentVariable("RENDER_LLM_TEST_CANCEL_EXPORT")=="early")TestCancel();
 }
 void SetRunning(bool on){start->setEnabled(!on);kind->setEnabled(!on);resolution->setEnabled(!on);width->setEnabled(!on);height->setEnabled(!on);samples->setEnabled(!on);frames->setEnabled(!on&&kind->currentIndex()!=0);fps->setEnabled(!on&&kind->currentIndex()!=0);progress->setVisible(on);cancel->setVisible(on);cancel->setEnabled(on);}
 void Read(const QByteArray& line){
  const auto obj=QJsonDocument::fromJson(line).object();
  if(obj.value("type")=="cancelled"){cancelled=true;status->setText("Cancelled · your scene is unchanged");return;}
  if(obj.value("type")=="phase"){
   const QString phase=obj.value("phase").toString(),detail=obj.value("detail").toString();
   if(phase=="render"&&qEnvironmentVariableIsSet("RENDER_LLM_TEST_CANCEL_EXPORT")&&!testDispatched)
    QTimer::singleShot(1200,this,[this](){TestCancel();});
   if(cancelRequested)return;
   status->setText((phase=="render"?"Rendering":phase=="done"?"Verifying output":"Preparing renderer")+(detail.isEmpty()?QString():QString(" · ")+detail));return;
  }
  if(obj.value("type")!="result")return;
  finalResult=obj;
  const bool sequence=kind->currentIndex()==2;
  const QString relative=obj.value(kind->currentIndex()==0?"image":sequence?"sequence":"video").toString();
  const QString candidate=QDir::cleanPath(QDir(dataDir).filePath(relative));
  if(relative.isEmpty()||!candidate.startsWith(QDir::cleanPath(dataDir)+"/")||(sequence?!QFileInfo(candidate).isDir():!QFileInfo(candidate).isFile()||QFileInfo(candidate).size()==0)){status->setText("Export returned an invalid output path.");return;}
  output=candidate;open->setEnabled(true);
  status->setText(QString("Complete · %1 / %2").arg(obj.value("backend").toString(),obj.value("denoiser").toString()));
  if(kind->currentIndex()==0){QPixmap pix(output);if(pix.isNull()){output.clear();open->setEnabled(false);status->setText("The rendered image could not be decoded.");}else preview->setPixmap(pix.scaled(preview->size(),Qt::KeepAspectRatio,Qt::SmoothTransformation));}
  else preview->setText(QString("%1 frames\n%2 fps\n%3 ready").arg(obj.value("frames").toInt()).arg(obj.value("fps").toInt()).arg(sequence?"PNG sequence":"MP4"));
 }
 QJsonObject finalResult;
 QDateTime testStarted;QStringList partialFrames;int partialPolls=0;
 QComboBox *kind,*resolution;QSpinBox *width,*height,*samples,*frames,*fps;QLabel *preview,*status,*summary;QProgressBar* progress;QPushButton *start,*cancel,*open;QScrollArea* settings;
 QProcess* process=nullptr;QByteArray buffer,errors;QString dataDir,output,beforeDocument;
 bool cancelled=false,cancelRequested=false,closeRequested=false,editorCloseRequested=false,testDispatched=false,visibleWhileCancelling=false;
};
QPointer<ExportDialog> active;
}
void Export(bool video){if(active){active->raise();active->activateWindow();return;}active=new ExportDialog(video,QApplication::activeWindow());active->show();}
bool StopExportBeforeExit(){return active&&active->StopForEditorExit();}
}

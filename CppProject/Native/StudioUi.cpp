#include "StudioUi.hpp"
#include "Generated/Scripts.hpp"
#include "AppHandler.hpp"
#include "Asset/Sprite.hpp"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QSlider>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTimer>
#include <QComboBox>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QGridLayout>
#include <QUuid>
#include <QSaveFile>
#include <cmath>

namespace CppProject::StudioUi {
namespace {
QImage Icon(const QString& name, QSize size) {
 QImage result(size, QImage::Format_ARGB32_Premultiplied); result.fill(Qt::transparent);
 QPainter p(&result); p.setRenderHint(QPainter::Antialiasing);
 p.translate(size.width()/2., size.height()/2.); p.scale(.78,.78);
 p.setPen(QPen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
 auto line=[&](double a,double b,double c,double d){p.drawLine(QPointF(a,b),QPointF(c,d));};
 auto box=[&](double a,double b,double c,double d){p.drawRoundedRect(QRectF(a,b,c,d),2,2);};
 auto poly=[&](std::initializer_list<QPointF> pts){QPolygonF q; for(auto v:pts)q<<v; p.drawPolyline(q);};
 if(name.contains("CLOSE") || name=="CANCEL") {line(-5,-5,5,5);line(-5,5,5,-5);}
 else if(name.contains("CHEVRON") || name.contains("ARROW")) {double r=name.contains("LEFT")?90:name.contains("UP")?180:name.contains("RIGHT")?-90:0;p.rotate(r);poly({{-5,-2},{0,3},{5,-2}});}
 else if(name.contains("PLUS") || name.contains("ADD")) {line(-6,0,6,0);line(0,-6,0,6);}
 else if(name.contains("SEARCH")) {p.drawEllipse(QPointF(-1,-1),5,5);line(3,3,7,7);}
 else if(name.contains("CONE")) {poly({{0,-8},{-7,6},{7,6},{0,-8}});p.drawArc(QRectF(-7,3,14,6),180*16,180*16);}
 else if(name.contains("CYLINDER")) {p.drawEllipse(QRectF(-7,-7,14,5));line(-7,-4,-7,5);line(7,-4,7,5);p.drawArc(QRectF(-7,2,14,6),180*16,180*16);}
 else if(name.contains("PLANE")) {poly({{-8,2},{2,-5},{8,-2},{-2,5},{-8,2}});}
 else if(name.contains("STOPWATCH")) {p.drawEllipse(QPointF(0,1),6,6);line(-2,-8,2,-8);line(0,-8,0,-5);line(0,-2,0,2);}
 else if(name.contains("REPEAT")) {poly({{-7,1},{-7,-4},{7,-4},{4,-7}});poly({{7,-1},{7,4},{-7,4},{-4,7}});}
 else if(name.contains("INFO") || name.contains("HELP")) {p.drawEllipse(QPointF(0,0),7,7);line(0,-1,0,4);p.drawPoint(0,-4);}
 else if(name.contains("COPY") || name.contains("DUPLICATE")) {box(-7,-7,10,10);box(-3,-3,10,10);}
 else if(name.contains("PENCIL") || name.contains("EDIT")) {poly({{-6,6},{-5,1},{4,-8},{8,-4},{-1,5},{-6,6}});}
 else if(name.contains("KEY")) {poly({{0,-6},{6,0},{0,6},{-6,0},{0,-6}});}
 else if(name.contains("MAGNET")) {p.drawArc(QRectF(-6,-2,12,10),180*16,180*16);line(-6,-2,-6,-7);line(6,-2,6,-7);}
 else if(name.contains("EASE")) {QPainterPath path;path.moveTo(-7,6);path.cubicTo(3,6,-3,-6,7,-6);p.drawPath(path);}
 else if(name.contains("CAMERA")) {box(-9,-5,18,13);poly({{-5,-5},{-3,-8},{3,-8},{5,-5}});p.drawEllipse(QPointF(0,1),4,4);}
 else if(name.contains("PLAY") || name.contains("NEXT") || name.contains("PREVIOUS")) {if(name.contains("PREVIOUS"))p.rotate(180);poly({{-4,-6},{5,0},{-4,6},{-4,-6}});if(!name.contains("PLAY"))line(7,-6,7,6);}
 else if(name.contains("PAUSE")) {line(-3,-6,-3,6);line(3,-6,3,6);}
 else if(name=="STOP") {box(-5,-5,10,10);}
 else if(name.contains("MOVE") || name.contains("TRANSFORM")) {line(-8,0,8,0);line(0,-8,0,8);poly({{-5,-3},{-8,0},{-5,3}});poly({{5,-3},{8,0},{5,3}});poly({{-3,-5},{0,-8},{3,-5}});}
 else if(name.contains("ROTATE") || name.contains("RESET") || name.contains("REDO") || name.contains("UNDO")) {p.drawArc(QRectF(-6,-6,12,12),35*16,290*16);poly({{3,-7},{7,-7},{7,-3}});}
 else if(name.contains("SCALE") || name.contains("EXTERNAL")) {box(-7,-3,10,10);line(-1,1,7,-7);poly({{2,-7},{7,-7},{7,-2}});}
 else if(name.contains("PIN")) {p.rotate(35);poly({{-4,-7},{4,-7},{3,1},{6,3},{-6,3},{-3,1},{-4,-7}});line(0,3,0,9);}
 else if(name.contains("FOLDER")) {poly({{-8,-3},{-8,-6},{-2,-6},{0,-3},{8,-3},{7,7},{-7,7},{-8,-3}});}
 else if(name.contains("SAVE") || name.contains("EXPORT") || name.contains("DOWNLOAD")) {line(0,-8,0,3);poly({{-4,-1},{0,3},{4,-1}});poly({{-7,3},{-7,7},{7,7},{7,3}});}
 else if(name.contains("DELETE")) {line(-6,-5,6,-5);poly({{-5,-5},{-4,7},{4,7},{5,-5}});line(-2,-8,2,-8);line(-1,-2,-1,4);line(2,-2,2,4);}
 else if(name.contains("KEYFRAME")) {poly({{0,-6},{6,0},{0,6},{-6,0},{0,-6}});}
 else if(name.contains("CHECK") || name.contains("TICK")) {poly({{-6,0},{-2,4},{6,-5}});}
 else if(name.contains("IMAGE") || name.contains("TEXTURE")) {box(-8,-7,16,14);poly({{-6,4},{-1,-1},{3,3},{5,1},{7,4}});p.drawEllipse(QPointF(3,-3),1,1);}
 else if(name.contains("WORLD") || name.contains("SPHERE")) {p.drawEllipse(QPointF(0,0),7,7);p.drawEllipse(QRectF(-3,-7,6,14));line(-7,0,7,0);}
 else if(name.contains("CUBE") || name.contains("SHAPE") || name.contains("ASSET")) {poly({{0,-8},{7,-4},{7,4},{0,8},{-7,4},{-7,-4},{0,-8}});poly({{-7,-4},{0,0},{7,-4}});line(0,0,0,8);}
 else if(name.contains("LIGHT") || name.contains("SUN")) {p.drawEllipse(QPointF(0,0),3,3);for(int i=0;i<8;i++){p.rotate(45);line(0,6,0,8);}}
 else if(name.contains("LIST") || name.contains("MENU") || name.contains("HIERARCHY")) {for(int i=-1;i<=1;i++){line(-3,i*5,7,i*5);p.drawPoint(-7,i*5);}}
 else if(name.contains("VOLUME")) {poly({{-7,-3},{-3,-3},{2,-7},{2,7},{-3,3},{-7,3},{-7,-3}});p.drawArc(QRectF(-1,-5,10,10),-65*16,130*16);}
 else if(name.contains("CLOCK") || name.contains("TIME")) {p.drawEllipse(QPointF(0,0),7,7);poly({{0,-4},{0,0},{4,2}});}
 else if(name.contains("WARNING") || name.contains("ERROR")) {poly({{0,-8},{8,6},{-8,6},{0,-8}});line(0,-3,0,1);p.drawPoint(0,4);}
 else if(name.contains("EYE") || name.contains("VISIBLE")) {poly({{-9,0},{-4,-5},{4,-5},{9,0},{4,5},{-4,5},{-9,0}});p.drawEllipse(QPointF(0,0),2,2);}
 else if(name.contains("LOCK")) {box(-5,-1,10,9);p.drawArc(QRectF(-4,-8,8,12),0,180*16);}
 else if(name.contains("VIEWPORT") || name.contains("LETTERBOX")) {box(-8,-6,16,12);line(0,-6,0,6);}
 else if(name.contains("GRID")) {box(-7,-7,14,14);line(-7,0,7,0);line(0,-7,0,7);}
 else { // Every remaining command receives a newly authored neutral control glyph.
  line(-7,-5,7,-5);line(-7,1,7,1);line(-7,7,7,7);
  p.setBrush(Qt::white);p.drawEllipse(QPointF(-2,-5),2,2);p.drawEllipse(QPointF(4,1),2,2);p.drawEllipse(QPointF(-4,7),2,2);
 }
 return result;
}
}
QString StyleSheet() { return QStringLiteral(
 "QWidget{font-family:'Segoe UI';font-size:13px;}QDialog,QMessageBox{background:#171719;color:#eeeae4;}"
 "QLabel,QCheckBox,QRadioButton{color:#ddd9d2;} QLabel[role=title]{font-size:24px;font-weight:600;}"
 "QLineEdit,QSpinBox,QDoubleSpinBox,QComboBox,QListWidget,QTextEdit{background:#242426;color:#eeeae4;border:0;border-radius:6px;padding:9px;selection-background-color:#555451;}"
 "QLineEdit:focus,QSpinBox:focus,QComboBox:focus{border:1px solid #bbb7ad;}"
 "QComboBox::drop-down{border:0;width:26px;}QComboBox QAbstractItemView{background:#242426;color:#eeeae4;selection-background-color:#44433f;padding:6px;border:0;}"
 "QSpinBox::up-button,QSpinBox::down-button{border:0;background:transparent;width:24px;}"
 "QPushButton{background:#303032;color:#eae7e1;border:0;border-radius:6px;padding:10px 18px;}"
 "QPushButton:hover{background:#414143;} QPushButton:default{background:#e5dfd3;color:#171719;} QPushButton:disabled{color:#767574;}"
 "QSlider::groove:horizontal{height:4px;background:#38383b;border-radius:2px;}"
 "QSlider::handle:horizontal{background:#e5dfd3;width:14px;margin:-5px 0;border-radius:7px;}"
 "QProgressBar{background:#303032;color:#eeeae4;border:0;border-radius:4px;text-align:center;}QProgressBar::chunk{background:#b8b4a9;}"
 "QTabWidget::pane{border:0;}QTabBar::tab{background:#242426;color:#bbb7ad;padding:10px 20px;}QTabBar::tab:selected{background:#3c3b38;color:#fff;}"
 "QScrollBar:vertical{background:transparent;width:8px;margin:0;}QScrollBar::handle:vertical{background:#494949;min-height:32px;border-radius:4px;}"
 "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}"
 ); }
void Surface(double x,double y,double w,double h,IntType color,double alpha,double radius) {
 if(w<=0||h<=0||alpha<=0)return;
 const double r=std::min(radius,std::min(w,h)/2.);
 draw_primitive_begin(pr_trianglefan);
 draw_vertex_color(x+w/2,y+h/2,color,alpha*draw_get_alpha());
 for(int corner=0;corner<4;corner++) for(int i=0;i<=6;i++) {
  const double angle=(-90.+corner*90.+i*15.)*3.141592653589793/180.;
  const double cx=(corner==0||corner==1)?x+w-r:x+r;
  const double cy=(corner<2)?(corner==0?y+r:y+h-r):(corner==2?y+h-r:y+r);
  draw_vertex_color(cx+std::cos(angle)*r,cy+std::sin(angle)*r,color,alpha*draw_get_alpha());
 }
 draw_vertex_color(x+w-r,y,color,alpha*draw_get_alpha());draw_primitive_end();
}
void Field(double x,double y,double w,double h,bool focused,bool error) {
 Surface(x,y,w,h,0x282727,1,6);
 if(focused||error)Surface(x+8,y+h-2,w-16,1,error?0x7373df:0xc9c4bb,1,0);
}
void Toggle(double x,double y,bool on,double alpha) {
 Surface(x,y,26,14,on?0xc4c0b6:0x484646,alpha,7);
 Surface(x+(on?14:2),y+2,10,10,on?0x222222:0xb7b3af,alpha,5);
}
void Install() {
 const char* names[] = {
#include "StudioIconNames.inc"
 };
 if(auto sprite=FindSprite(ID_spr_icons)) {
  for(int i=0;i<sprite->frames.size();i++) {delete sprite->frames[i];sprite->frames[i]=new Sprite::Frame(Icon(i<int(sizeof(names)/sizeof(names[0]))?names[i]:"CONTROL",sprite->size),true);}
 }
 if(auto sprite=FindSprite(ID_spr_checkbox_tick))for(auto& f:sprite->frames){delete f;f=new Sprite::Frame(Icon("TICK",sprite->size),true);}
 for(const IntType id : {ID_spr_chevron_ani, ID_spr_arrow_ani, ID_spr_arrow_small_ani, ID_spr_arrow_up_down_ani}) {
  if(auto sprite=FindSprite(id)) for(int i=0;i<sprite->frames.size();++i) {
   QImage frame(sprite->size,QImage::Format_ARGB32_Premultiplied);frame.fill(Qt::transparent);
   QPainter p(&frame);p.setRenderHint(QPainter::Antialiasing);p.translate(sprite->size.width()/2.,sprite->size.height()/2.);
   p.rotate(-90.+90.*i/std::max(1,int(sprite->frames.size())-1));p.setPen(QPen(Qt::white,1.6,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
   QPolygonF chevron;chevron<<QPointF(-4,-2)<<QPointF(0,2)<<QPointF(4,-2);p.drawPolyline(chevron);p.end();
   delete sprite->frames[i];sprite->frames[i]=new Sprite::Frame(frame,true);
  }
 }
 qApp->setStyleSheet(StyleSheet());
 QPalette palette=qApp->palette();
 palette.setColor(QPalette::Window,QColor("#171719"));palette.setColor(QPalette::Base,QColor("#242426"));
 palette.setColor(QPalette::Button,QColor("#303032"));palette.setColor(QPalette::Text,QColor("#eeeae4"));
 palette.setColor(QPalette::WindowText,QColor("#eeeae4"));palette.setColor(QPalette::ButtonText,QColor("#eeeae4"));
 palette.setColor(QPalette::Highlight,QColor("#555451"));palette.setColor(QPalette::HighlightedText,Qt::white);qApp->setPalette(palette);
 qApp->setWindowIcon(QIcon(":/Icons/studio-camera.png"));
}
void ColorDialog(IntType script,IntType color) {
 // Use the existing action once on acceptance; cancellation never mutates history.
 PickColor(color,[script](IntType value){QTimer::singleShot(0,[=](){script_execute(ScopeAny(global::_app->id),{script,value});});});
}
void PickColor(IntType color,std::function<void(IntType)> accepted) {
 auto dialog=new QDialog(QApplication::activeWindow());dialog->setAttribute(Qt::WA_DeleteOnClose);
 dialog->setWindowTitle("Surface color");dialog->setMinimumWidth(350);dialog->setStyleSheet(StyleSheet());
 auto layout=new QVBoxLayout(dialog);layout->setContentsMargins(28,24,28,24);layout->setSpacing(18);
 auto title=new QLabel("Choose a color");title->setProperty("role","title");layout->addWidget(title);
 auto preview=new QLabel;preview->setFixedHeight(72);layout->addWidget(preview);
 auto hex=new QLineEdit(QColor(color&255,(color>>8)&255,(color>>16)&255).name().toUpper());layout->addWidget(hex);
 auto sliders=new QWidget;auto rows=new QVBoxLayout(sliders);rows->setContentsMargins(0,0,0,0);
 QSlider* channel[3];for(int i=0;i<3;i++){auto row=new QHBoxLayout;row->addWidget(new QLabel(QStringList{"Red","Green","Blue"}[i]));channel[i]=new QSlider(Qt::Horizontal);channel[i]->setRange(0,255);channel[i]->setValue((color>>(i*8))&255);row->addWidget(channel[i]);rows->addLayout(row);}layout->addWidget(sliders);
 auto update=[=](){QColor c(channel[0]->value(),channel[1]->value(),channel[2]->value());hex->setText(c.name().toUpper());preview->setStyleSheet("background:"+c.name()+";border-radius:8px;");};
 for(auto slider:channel)QObject::connect(slider,&QSlider::valueChanged,dialog,update);
 QObject::connect(hex,&QLineEdit::editingFinished,dialog,[=](){QColor c(hex->text());if(c.isValid()){channel[0]->setValue(c.red());channel[1]->setValue(c.green());channel[2]->setValue(c.blue());}});update();
 auto buttons=new QDialogButtonBox(QDialogButtonBox::Cancel|QDialogButtonBox::Ok);buttons->button(QDialogButtonBox::Ok)->setText("Apply color");buttons->button(QDialogButtonBox::Ok)->setDefault(true);layout->addWidget(buttons);
 QObject::connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
 QObject::connect(buttons,&QDialogButtonBox::accepted,dialog,[=](){QColor c(hex->text());if(!c.isValid())return;const IntType value=c.red()|(c.green()<<8)|(c.blue()<<16);dialog->accept();accepted(value);});
 dialog->setWindowModality(Qt::ApplicationModal);dialog->show();
 if(qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_COLOR_UI"))QTimer::singleShot(700,dialog,[=](){dialog->grab().save(QDir::current().filePath("Data/studio-color-ui.png"));});
 if(qEnvironmentVariableIsSet("RENDER_LLM_TEST_COLOR")) {
  const QString before=renderllm_document_snapshot().QStr();const bool apply=qEnvironmentVariable("RENDER_LLM_TEST_COLOR")=="apply";
  QTimer::singleShot(300,dialog,[=](){
   const bool wasModal=HasModal();hex->setText("#123456");
   buttons->button(apply?QDialogButtonBox::Ok:QDialogButtonBox::Cancel)->click();
   QTimer::singleShot(150,qApp,[=](){
    const QString after=renderllm_document_snapshot().QStr();
    const QString world=QJsonDocument::fromJson(after.toUtf8()).object().value("scene").toObject().value("world").toObject().value("color").toString();
    qApp->exit(wasModal&&!HasModal()&&(apply?world=="#123456":before==after)?0:2);
   });
  });
 }
}
bool HasModal() { return QApplication::activeModalWidget() != nullptr; }
void Workspace() {
 ScopeAny app(global::_app->id);
 const IntType left=DsMap(global::_app->panel_map).Value("left"),right=DsMap(global::_app->panel_map).Value("right");
 for(IntType tab : {global::_app->properties,global::_app->ground_editor,global::_app->frame_editor,global::_app->timeline_editor,global::_app->template_editor}) {
  tab_close(app,tab);ObjType(obj_tab,tab)->panel=left;ObjType(obj_tab,tab)->panel_last=left;
 }
 tab_close(app,global::_app->assistant);ObjType(obj_tab,global::_app->assistant)->panel=right;
 ObjType(obj_tab,global::_app->assistant)->panel_last=right;tab_show(app,global::_app->assistant);
 ObjType(obj_panel,left)->size=300;ObjType(obj_panel,right)->size=360;
 ObjType(obj_view,global::_app->view_main)->camera=-5; // Follow the scene's active camera, with work-view fallback.
 global::_app->setting_assistant_location="right";global::_app->setting_properties_location="left";
 global::_app->update_interface_wait=true;
}
void AddObject() {
 auto dialog=new QDialog(QApplication::activeWindow());dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Add to scene");dialog->setMinimumWidth(480);
 auto layout=new QVBoxLayout(dialog);layout->setContentsMargins(28,24,28,24);layout->setSpacing(18);
 auto title=new QLabel("What are we creating?");title->setProperty("role","title");layout->addWidget(title);
 auto name=new QLineEdit;name->setPlaceholderText("Name · optional");name->setMaxLength(80);layout->addWidget(name);
 auto grid=new QGridLayout;grid->setSpacing(12);layout->addLayout(grid);
 auto status=new QLabel;status->setWordWrap(true);layout->addWidget(status);
 const QStringList kinds={"cube","sphere","cylinder","cone","plane","camera","point"};
 const QStringList labels={"Box","Sphere","Cylinder","Cone","Panel · vertical","Camera","Studio light"};
 for(int i=0;i<kinds.size();i++) {
  const QString kind=kinds[i],label=labels[i];auto button=new QPushButton(label);button->setMinimumHeight(54);button->setIcon(QPixmap::fromImage(Icon(kind=="point"?"LIGHT":kind.toUpper(),QSize(24,24))));grid->addWidget(button,i/2,i%2);
  QObject::connect(button,&QPushButton::clicked,dialog,[=](){
   auto doc=QJsonDocument::fromJson(renderllm_document_snapshot().QStr().toUtf8()).object();
   if(doc.isEmpty()){status->setText(renderllm_document_error().QStr());return;}
   auto scene=doc.value("scene").toObject();auto objects=scene.value("objects").toArray();auto materials=doc.value("materials").toObject();
   const QString id=kind+"_"+QUuid::createUuid().toString(QUuid::Id128).left(10);
   const bool camera=kind=="camera",light=kind=="point";
   QJsonObject object{{"id",id},{"name",name->text().trimmed().isEmpty()?label:name->text().trimmed()},{"kind",kind},{"visible",true},
    {"position",camera?QJsonArray{10,10,10}:light?QJsonArray{-4,3,8}:QJsonArray{0,0,0}},
    {"rotation",camera?QJsonArray{45,0,225}:QJsonArray{0,0,0}},
    {"scale",camera||light?QJsonArray{1,1,1}:QJsonArray{.25,.25,.25}}};
   if(camera){object.insert("camera",QJsonObject{{"fov_y_deg",45},{"orthographic",false},{"ortho_height",128},{"dof",false},{"focus_distance",0}});scene.insert("active_camera_id",id);}
   else if(light)object.insert("light",QJsonObject{{"color","#FFFFFF"},{"power",3},{"size",3}});
   else {const QString mat=id+"_surface";object.insert("material_id",mat);materials.insert(mat,QJsonObject{{"color","#C9C7C2"},{"metallic",0},{"roughness",.5},{"emission",0}});}
   objects.append(object);scene.insert("objects",objects);doc.insert("scene",scene);doc.insert("materials",materials);
   if(!renderllm_document_apply(StringType(QString::fromUtf8(QJsonDocument(doc).toJson(QJsonDocument::Compact))))){status->setText(renderllm_document_error().QStr());return;}
   dialog->accept();
  });
  if(qEnvironmentVariable("RENDER_LLM_TEST_CREATE_KIND")==kind)QTimer::singleShot(300,dialog,[=](){
   global::_app->background_ambient_color=0x332211;global::_app->project_render_exposure=1.75;
   button->click();const QByteArray snapshot=renderllm_document_snapshot().QStr().toUtf8();
   const auto objects=QJsonDocument::fromJson(snapshot).object().value("scene").toObject().value("objects").toArray();
   const bool created=!objects.isEmpty()&&objects.last().toObject().value("kind").toString()==kind
    && global::_app->background_ambient_color.ToInt()==0x332211 && global::_app->project_render_exposure.ToReal()==1.75;
   QSaveFile evidence(QDir::current().filePath("Data/studio-created.json"));
   const bool saved=evidence.open(QIODevice::WriteOnly)&&evidence.write(snapshot)==snapshot.size()&&evidence.commit();
   qApp->exit(created&&saved?0:2);
  });
 }
 dialog->setWindowModality(Qt::ApplicationModal);dialog->show();
 if(qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_ADD_UI"))QTimer::singleShot(700,dialog,[=](){dialog->grab().save(QDir::current().filePath("Data/studio-add-ui.png"));});
}
void AgentSettings() {
 auto dialog=new QDialog(QApplication::activeWindow());dialog->setAttribute(Qt::WA_DeleteOnClose);
 dialog->setWindowTitle("Agent preferences");dialog->setMinimumWidth(400);
 auto layout=new QVBoxLayout(dialog);layout->setContentsMargins(28,24,28,24);layout->setSpacing(18);
 auto title=new QLabel("Your agent");title->setProperty("role","title");layout->addWidget(title);
 auto note=new QLabel("Uses your connected Codex account. Changes apply to the next message.");note->setWordWrap(true);layout->addWidget(note);
 layout->addWidget(new QLabel("Model"));auto model=new QComboBox;
 model->addItem("Luna · quick edits","gpt-5.6-luna");model->addItem("Terra · balanced","gpt-5.6-terra");model->addItem("Astra · complex scenes","gpt-6-astra");
 model->setCurrentIndex(model->findData(global::mi3_as_model.ToStr().QStr()));layout->addWidget(model);
 auto advanced=new QPushButton("Reasoning options");advanced->setCheckable(true);layout->addWidget(advanced);
 auto effort=new QComboBox;for(const auto& level:QStringList{"low","medium","high","xhigh","max"})effort->addItem(level,level);
 effort->setCurrentIndex(effort->findData(global::mi3_as_effort.ToStr().QStr()));effort->hide();layout->addWidget(effort);
 QObject::connect(advanced,&QPushButton::toggled,effort,&QWidget::setVisible);
 auto buttons=new QDialogButtonBox(QDialogButtonBox::Cancel|QDialogButtonBox::Save);buttons->button(QDialogButtonBox::Save)->setDefault(true);layout->addWidget(buttons);
 QObject::connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
 QObject::connect(buttons,&QDialogButtonBox::accepted,dialog,[=](){
  if(model->currentIndex()<0)return;
  action_assistant_model(ScopeAny(global::_app->id),StringType(model->currentData().toString()));
  if(effort->currentIndex()>=0)action_assistant_effort(ScopeAny(global::_app->id),StringType(effort->currentData().toString()));
  dialog->accept();
 });
 dialog->setWindowModality(Qt::ApplicationModal);dialog->show();
}
int Message(const QString& title, const QString& text, const QStringList& buttons, int safeChoice) {
 QDialog dialog(QApplication::activeWindow());dialog.setWindowTitle("render-llm");dialog.setMinimumWidth(460);
 auto layout=new QVBoxLayout(&dialog);layout->setContentsMargins(28,28,28,28);layout->setSpacing(24);
 auto heading=new QLabel(title);heading->setProperty("role","title");layout->addWidget(heading);
 auto body=new QLabel(text);body->setTextFormat(Qt::PlainText);body->setWordWrap(true);body->setMaximumWidth(560);layout->addWidget(body);
 auto actions=new QHBoxLayout;actions->setSpacing(12);actions->addStretch();layout->addLayout(actions);
 int choice=safeChoice;
 for(int i=0;i<buttons.size();++i)if(!buttons[i].isEmpty()) {
  auto button=new QPushButton(buttons[i]);button->setDefault(i==safeChoice);actions->addWidget(button);
  QObject::connect(button,&QPushButton::clicked,&dialog,[&,i](){choice=i;dialog.accept();});
 }
 if(qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_ALERT_UI")) {
  QTimer::singleShot(500,&dialog,[&](){dialog.grab().save(QDir::current().filePath("Data/studio-alert-ui.png"));});
  QTimer::singleShot(800,&dialog,&QDialog::reject);
 }
 App->ExecDialog(&dialog);return choice;
}
}

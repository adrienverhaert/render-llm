#include "StudioUi.hpp"
#include "StudioWidgets.hpp"
#include "RenderLlmSelection.hpp"
#include "RenderLlmProjectDialog.hpp"
#include "Generated/Scripts.hpp"
#include "Generated/GmlFunc.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <functional>
#include <vector>

namespace CppProject::StudioUi {
namespace {
// A key inspector, not a second animation engine. Draft tracks go through the
// existing document transaction; the retained timeline evaluates all easing.
class KeyStrip final : public QWidget {
public:
    QJsonArray keys;
    int frames=96, selected=-1;
    std::function<void(int)> choose;
    std::function<void(int,int)> retime;
    explicit KeyStrip(QWidget* parent=nullptr):QWidget(parent) {setMinimumHeight(128);setFocusPolicy(Qt::StrongFocus);setAccessibleName("Motion keyframes");}
    double X(int frame) const {return 24.+frame*std::max(1,width()-48)/double(std::max(1,frames-1));}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),QColor("#101012"));
        p.setPen(QColor("#77756f"));
        for(int i=0;i<=8;++i){const int frame=qRound((frames-1)*i/8.);const double x=X(frame);p.drawLine(QPointF(x,36),QPointF(x,92));p.drawText(QRectF(x-22,9,44,20),Qt::AlignCenter,QString::number(frame));}
        for(int i=0;i<keys.size();++i){const auto key=keys[i].toObject();const double x=X(key.value("frame").toInt());p.setPen(Qt::NoPen);p.setBrush(i==selected?QColor("#f0e8d8"):QColor("#96928a"));
            if(key.value("easing")=="instant")p.drawRect(QRectF(x-5,60,10,10));
            else {QPolygonF diamond;diamond<<QPointF(x,57)<<QPointF(x+7,65)<<QPointF(x,73)<<QPointF(x-7,65);p.drawPolygon(diamond);}
        }
        if(keys.isEmpty()){p.setPen(QColor("#aaa69c"));p.drawText(rect().adjusted(10,90,-10,0),Qt::AlignCenter,"No keys on this property · add one below");}
    }
    void mousePressEvent(QMouseEvent* e) override {
        if(e->button()!=Qt::LeftButton)return;
        for(int i=0;i<keys.size();++i)if(std::abs(e->pos().x()-X(keys[i].toObject().value("frame").toInt()))<=10&&std::abs(e->pos().y()-65)<=14){press=e->pos();if(choose)choose(i);drag=selected;return;}
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if(drag<0)return;const int index=drag;drag=-1;
        if((e->pos()-press).manhattanLength()<4)return;
        const int frame=std::clamp(qRound((e->pos().x()-24.)*(frames-1)/std::max(1,width()-48)),0,frames-1);
        if(retime)retime(index,frame);
    }
private:
    int drag=-1;QPoint press;
};

QString Label(QString property) {
    static const QMap<QString,QString> names={{"position","Position"},{"rotation","Rotation"},{"scale","Scale"},{"visible","Visible"},
        {"material.color","Surface color"},{"material.metallic","Metallic"},{"material.roughness","Roughness"},{"material.emission","Emission"},
        {"camera.fov_y_deg","Field of view"},{"camera.ortho_height","Orthographic height"},{"camera.focus_distance","Focus distance"},{"camera.dof","Depth of field"},{"camera.projection","Projection"},
        {"light.color","Light color"},{"light.power","Light power"},{"light.size","Light size"},{"world.color","Background color"},{"world.strength","Environment strength"}};
    return names.value(property,property);
}
class MotionDialog final : public QDialog {
public:
    explicit MotionDialog(QWidget* parent):QDialog(parent) {
        original=QJsonDocument::fromJson(renderllm_document_snapshot().ToUtf8()).object();draft=original;revision=renderllm_document_revision();
        setAttribute(Qt::WA_DeleteOnClose);setWindowTitle("Motion · render-llm");setStyleSheet(StyleSheet());resize(880,620);setWindowModality(Qt::ApplicationModal);
        auto outer=new QVBoxLayout(this);outer->setContentsMargins(0,0,0,0);outer->setSpacing(0);
        auto scroller=new QScrollArea;scroller->setWidgetResizable(true);scroller->setFrameShape(QFrame::NoFrame);outer->addWidget(scroller,1);
        auto content=new QWidget;scroller->setWidget(content);auto root=new QVBoxLayout(content);root->setContentsMargins(28,24,28,16);root->setSpacing(16);
        auto title=new QLabel("Shape the movement");title->setProperty("role","title");root->addWidget(title);
        auto header=new QHBoxLayout;target=new QComboBox;property=new QComboBox;header->addWidget(target,1);header->addWidget(property,1);root->addLayout(header);
        for(const auto& item:original.value("scene").toObject().value("objects").toArray()){const auto obj=item.toObject();target->addItem(obj.value("name").toString(),obj.value("id").toString());}
        target->addItem("Environment","$world");target->setAccessibleName("Animation object");property->setAccessibleName("Animation property");
        QString prior=RenderLlmSelectedObject();if(prior.isEmpty()&&!Tracks().isEmpty())prior=Tracks()[0].toObject().value("target").toString();if(target->findData(prior)>=0)target->setCurrentIndex(target->findData(prior));
        auto timing=new QHBoxLayout;count=new QSpinBox;count->setRange(1,100000);count->setValue(Animation().value("frame_count").toInt(96));fps=new QSpinBox;fps->setRange(1,240);fps->setValue(Animation().value("fps").toInt(24));
        timing->addWidget(new QLabel("Duration · frames"));timing->addWidget(count);timing->addSpacing(24);timing->addWidget(new QLabel("Frames / second"));timing->addWidget(fps);timing->addStretch();root->addLayout(timing);
        strip=new KeyStrip;strip->frames=count->value();root->addWidget(strip);
        fields=new QWidget;form=new QFormLayout(fields);form->setContentsMargins(0,0,0,0);form->setHorizontalSpacing(20);form->setVerticalSpacing(12);root->addWidget(fields);
        frame=new QSpinBox;frame->setRange(0,99999);frame->setAccessibleName("Key frame");form->addRow("Frame",frame);
        valueHost=new QWidget;valueLayout=new QHBoxLayout(valueHost);valueLayout->setContentsMargins(0,0,0,0);form->addRow("Value",valueHost);
        easing=new QComboBox;easing->addItem("Linear","linear");easing->addItem("Hold","instant");easing->addItem("Smooth · neighboring keys","smooth");easing->addItem("Bézier","bezier");
        for(const auto& family:QStringList{"quad","cubic","quart","quint","sine","expo","circ","back","bounce","elastic"})for(const auto& direction:QStringList{"in","out","inout"})easing->addItem(family+" · "+direction,"ease"+direction+family);
        form->addRow("Transition",easing);handles=new QWidget;auto handleLayout=new QHBoxLayout(handles);handleLayout->setContentsMargins(0,0,0,0);
        for(int i=0;i<4;++i){bezier[i]=new QDoubleSpinBox;bezier[i]->setRange(-1000000,1000000);bezier[i]->setDecimals(5);bezier[i]->setPrefix(QStringList{"X1  ","Y1  ","X2  ","Y2  "}[i]);bezier[i]->setButtonSymbols(QAbstractSpinBox::NoButtons);handleLayout->addWidget(bezier[i]);}form->addRow("Bézier handles",handles);
        auto tools=new QHBoxLayout;add=new QPushButton("Add key at frame");remove=new QPushButton("Remove key");tools->addWidget(add);tools->addWidget(remove);tools->addStretch();root->addLayout(tools);
        auto hint=new QLabel("Drag a marker to retime it. The main timeline keeps playback, multi-selection and copy/paste. Transitions are shared by properties on the same object/frame.");hint->setWordWrap(true);hint->setStyleSheet("color:#a5a096;");root->addWidget(hint);root->addStretch();
        error=new QLabel;error->setWordWrap(true);error->setStyleSheet("color:#e8a399;");root->addWidget(error);
        auto footer=new QWidget;auto footerLayout=new QHBoxLayout(footer);footerLayout->setContentsMargins(28,12,28,24);auto buttons=new QDialogButtonBox(QDialogButtonBox::Cancel|QDialogButtonBox::Apply);footerLayout->addWidget(buttons);outer->addWidget(footer);apply=buttons->button(QDialogButtonBox::Apply);apply->setDefault(true);
        connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);connect(apply,&QPushButton::clicked,this,[this](){Apply();});
        connect(target,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){if(!SaveKey()){QSignalBlocker guard(target);target->setCurrentIndex(target->findData(targetId));return;}LoadTarget();});
        connect(property,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){if(!SaveKey()){QSignalBlocker guard(property);property->setCurrentIndex(property->findData(propertyId));return;}LoadTrack();});
        connect(easing,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](){const bool show=easing->currentData()=="bezier";handles->setVisible(show);form->labelForField(handles)->setVisible(show);});
        connect(count,qOverload<int>(&QSpinBox::valueChanged),this,[this](int n){strip->frames=n;strip->update();});
        connect(add,&QPushButton::clicked,this,[this](){Add();});connect(remove,&QPushButton::clicked,this,[this](){Remove();});
        strip->choose=[this](int index){const int at=Keys()[index].toObject().value("frame").toInt();const bool same=index==selected;if(!SaveKey())return;if(same){LoadKey(selected);return;}const auto keys=Keys();for(int i=0;i<keys.size();++i)if(keys[i].toObject().value("frame").toInt()==at){LoadKey(i);return;}};
        strip->retime=[this](int index,int at){if(!SaveKey())return;LoadKey(index);frame->setValue(at);if(SaveKey())Refresh();};
        LoadTarget();
        if(qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_MOTION_UI"))QTimer::singleShot(350,this,[this](){grab().save(QDir::current().filePath("Data/studio-motion-ui.png"));});
        if(qEnvironmentVariableIsSet("RENDER_LLM_TEST_MOTION"))QTimer::singleShot(500,this,[this](){Test();});
    }
private:
    QJsonObject Animation() const {return draft.value("animation").toObject();}
    QJsonArray Tracks() const {return Animation().value("tracks").toArray();}
    QJsonObject Object() const {for(const auto& entry:draft.value("scene").toObject().value("objects").toArray())if(entry.toObject().value("id")==targetId)return entry.toObject();return {};}
    int TrackIndex() const {const auto tracks=Tracks();for(int i=0;i<tracks.size();++i)if(tracks[i].toObject().value("target")==targetId&&tracks[i].toObject().value("property")==propertyId)return i;return -1;}
    QJsonArray Keys() const {const int index=TrackIndex();return index<0?QJsonArray():Tracks()[index].toObject().value("keys").toArray();}
    void SetKeys(QJsonArray keys) {
        auto tracks=Tracks();const int index=TrackIndex();
        if(keys.isEmpty()){if(index>=0)tracks.removeAt(index);}
        else {auto track=index>=0?tracks[index].toObject():QJsonObject{{"id",QString("track_%1").arg(nextId++)},{"target",targetId},{"property",propertyId}};
            if(index<0){bool collision=true;while(collision){collision=false;for(const auto& t:tracks)if(t.toObject().value("id")==track.value("id")){track.insert("id",QString("track_%1").arg(nextId++));collision=true;break;}}}
            track.insert("keys",keys);if(index<0)tracks.append(track);else tracks[index]=track;
        }
        auto animation=Animation();animation.insert("tracks",tracks);draft.insert("animation",animation);
    }
    QJsonValue BaseValue() const {
        if(targetId=="$world")return draft.value("scene").toObject().value("world").toObject().value(propertyId.mid(6));
        auto object=Object();if(!propertyId.contains('.'))return object.value(propertyId);
        if(propertyId.startsWith("material."))return draft.value("materials").toObject().value(object.value("material_id").toString()).toObject().value(propertyId.mid(9));
        if(propertyId=="camera.projection")return object.value("camera").toObject().value("orthographic").toBool()?"orthographic":"perspective";
        const auto parts=propertyId.split('.');return object.value(parts[0]).toObject().value(parts[1]);
    }
    void LoadTarget() {
        targetId=target->currentData().toString();QSignalBlocker guard(property);property->clear();
        QStringList props;if(targetId=="$world")props=QStringList{"world.color","world.strength"};else {
            props=QStringList{"position","rotation","visible"};const auto object=Object();
            if(object.contains("material_id"))props.append({"scale","material.color","material.metallic","material.roughness","material.emission"});
            if(object.contains("camera"))props.append({"camera.fov_y_deg","camera.ortho_height","camera.focus_distance","camera.dof","camera.projection"});
            if(object.contains("light"))props.append({"light.color","light.power","light.size"});
        }
        for(const auto& prop:props)property->addItem(Label(prop),prop);LoadTrack();
    }
    void LoadTrack() {propertyId=property->currentData().toString();selected=-1;Refresh();LoadKey(Keys().isEmpty()?-1:0);}
    void Refresh() {strip->keys=Keys();strip->selected=selected;strip->update();remove->setEnabled(selected>=0);}
    void LoadKey(int index) {
        selected=index;loaded=index>=0?Keys()[index].toObject():QJsonObject{{"frame",frame->value()},{"value",BaseValue()},{"easing","linear"}};
        frame->setValue(loaded.value("frame").toInt());easing->setCurrentIndex(easing->findData(loaded.value("easing").toString()));
        const auto points=loaded.value("bezier").toArray();for(int i=0;i<4;++i){bezier[i]->setValue(points.size()==4?points[i].toDouble():std::array<double,4>{.25,.1,.25,1}[i]);bezier[i]->setProperty("shown",bezier[i]->value());}
        handles->setVisible(easing->currentData()=="bezier");form->labelForField(handles)->setVisible(easing->currentData()=="bezier");
        while(auto item=valueLayout->takeAt(0)){if(item->widget())delete item->widget();delete item;}
        numbers.clear();boolean=nullptr;choice=nullptr;color=nullptr;const auto value=loaded.value("value");
        if(value.isBool()){boolean=new ToggleBox("Enabled");boolean->setChecked(value.toBool());valueLayout->addWidget(boolean);}
        else if(propertyId.endsWith(".color")){color=new QLineEdit(value.toString());color->setValidator(new QRegularExpressionValidator(QRegularExpression("#[0-9A-Fa-f]{6}"),color));auto pick=new QPushButton("Choose color");valueLayout->addWidget(color);valueLayout->addWidget(pick);
            connect(pick,&QPushButton::clicked,this,[this](){const QPointer<QLineEdit> field=color;QColor c(color->text());PickColor(c.red()|(c.green()<<8)|(c.blue()<<16),[field](IntType rgb){if(field)field->setText(QColor(rgb&255,(rgb>>8)&255,(rgb>>16)&255).name().toUpper());});});}
        else if(value.isString()){choice=new QComboBox;choice->addItem("Perspective","perspective");choice->addItem("Orthographic","orthographic");choice->setCurrentIndex(choice->findData(value.toString()));valueLayout->addWidget(choice);}
        else {const int size=value.isArray()?3:1;for(int i=0;i<size;++i){auto n=new QDoubleSpinBox;n->setDecimals(5);n->setRange(value.isArray()?-10000:0,value.isArray()?10000:1000000000);n->setButtonSymbols(QAbstractSpinBox::NoButtons);if(size==3)n->setPrefix(QString("%1  ").arg("XYZ"[i]));n->setValue(value.isArray()?value.toArray()[i].toDouble():value.toDouble());n->setProperty("shown",n->value());numbers.append(n);valueLayout->addWidget(n);}}
        Refresh();
    }
    bool ReadKey(QJsonObject& key) {
        key=loaded;key.insert("frame",frame->value());QJsonValue value=loaded.value("value");
        if(color){if(!color->hasAcceptableInput()){error->setText("Enter a six-digit color, for example #FFFFFF.");return false;}if(color->text()!=value.toString())value=color->text().toUpper();}
        if(boolean)value=boolean->isChecked();if(choice)value=choice->currentData().toString();
        if(!numbers.isEmpty()){if(value.isArray()){auto v=value.toArray();for(int i=0;i<3;++i)if(numbers[i]->value()!=numbers[i]->property("shown").toDouble())v[i]=numbers[i]->value();value=v;}else if(numbers[0]->value()!=numbers[0]->property("shown").toDouble())value=numbers[0]->value();}
        key.insert("value",value);key.insert("easing",easing->currentData().toString());
        if(easing->currentData()=="bezier"){auto points=loaded.value("bezier").toArray();if(points.size()!=4)points=QJsonArray{.25,.1,.25,1};for(int i=0;i<4;++i)if(bezier[i]->value()!=bezier[i]->property("shown").toDouble())points[i]=bezier[i]->value();key.insert("bezier",points);}else key.remove("bezier");return true;
    }
    void SyncTransition(const QJsonObject& key) {
        auto tracks=Tracks();for(int i=0;i<tracks.size();++i){auto track=tracks[i].toObject();if(track.value("target")!=targetId)continue;auto keys=track.value("keys").toArray();for(int j=0;j<keys.size();++j){auto other=keys[j].toObject();if(other.value("frame")!=key.value("frame"))continue;other.insert("easing",key.value("easing"));if(key.contains("bezier"))other.insert("bezier",key.value("bezier"));else other.remove("bezier");keys[j]=other;}track.insert("keys",keys);tracks[i]=track;}
        auto animation=Animation();animation.insert("tracks",tracks);draft.insert("animation",animation);
    }
    bool SaveKey() {
        if(selected<0)return true;QJsonObject key;if(!ReadKey(key))return false;auto keys=Keys();
        for(int i=0;i<keys.size();++i)if(i!=selected&&keys[i].toObject().value("frame")==key.value("frame")){error->setText("There is already a key at this frame. Choose another frame.");return false;}
        keys[selected]=key;std::vector<QJsonObject> sorted;for(const auto& k:keys)sorted.push_back(k.toObject());std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.value("frame").toInt()<b.value("frame").toInt();});keys={};for(const auto& k:sorted)keys.append(k);SetKeys(keys);
        if(key.value("easing")!=loaded.value("easing")||key.value("bezier")!=loaded.value("bezier")||key.value("frame")!=loaded.value("frame"))SyncTransition(key);
        for(int i=0;i<keys.size();++i)if(keys[i].toObject().value("frame")==key.value("frame"))selected=i;
        loaded=key;for(auto n:numbers)n->setProperty("shown",n->value());for(auto n:bezier)n->setProperty("shown",n->value());error->clear();return true;
    }
    void Add() {
        const int at=frame->value();QJsonObject key;if(!ReadKey(key))return;
        // Add duplicates the selected value, without moving its source key.
        auto keys=Keys();for(const auto& k:keys)if(k.toObject().value("frame").toInt()==at){error->setText("A key already exists here. Edit it or choose another frame.");return;}
        key.insert("frame",at);keys.append(key);SetKeys(keys);selected=keys.size()-1;loaded=key;if(!SaveKey())return;SyncTransition(key);LoadKey(selected);
    }
    void Remove() {if(selected<0)return;auto keys=Keys();keys.removeAt(selected);SetKeys(keys);selected=-1;LoadKey(keys.isEmpty()?-1:0);error->clear();}
    void Apply() {
        if(renderllm_document_revision()!=revision){error->setText("The scene changed. Reopen Motion before applying.");return;}
        if(!SaveKey())return;auto animation=Animation();animation.insert("fps",fps->value());animation.insert("frame_count",count->value());draft.insert("animation",animation);
        if(draft!=original&&!renderllm_document_apply(StringType(QJsonDocument(draft).toJson(QJsonDocument::Compact)))){error->setText(renderllm_document_error().QStr());return;}
        accept();
    }
    void Test();
    QJsonObject original,draft,loaded;IntType revision;QString targetId,propertyId;int selected=-1,nextId=1;
    QComboBox *target,*property,*easing,*choice=nullptr;QSpinBox *frame,*count,*fps;KeyStrip* strip;
    QWidget *fields,*valueHost,*handles;QFormLayout* form;QHBoxLayout* valueLayout;QDoubleSpinBox* bezier[4];QList<QDoubleSpinBox*> numbers;
    ToggleBox* boolean=nullptr;QLineEdit* color=nullptr;QLabel* error;QPushButton *add,*remove,*apply;
};
QPointer<MotionDialog> active;

void MotionDialog::Test() {
    const auto mode=qEnvironmentVariable("RENDER_LLM_TEST_MOTION");
    auto snapshot=[](){return QJsonDocument::fromJson(renderllm_document_snapshot().ToUtf8()).object();};
    auto select=[this](QString id,QString prop){target->setCurrentIndex(target->findData(id));property->setCurrentIndex(property->findData(prop));};
    select("sphere","position");bool passed=Keys().size()==2,mutated=false;
    auto evaluated=[&](){
        std::vector<double> result;const auto marker=global::_app->timeline_marker;
        RenderLlmSelectObject("sphere");const IntType timeline=global::tl_edit;
        for(int at: {0,6,12,18,24}){global::_app->timeline_marker=at;tl_update_values(ScopeAny(timeline));result.push_back(ObjType(obj_timeline,timeline)->value.Value(e_value_POS_Z).ToReal());}
        global::_app->timeline_marker=marker;tl_update_values(ScopeAny(timeline));return result;
    };
    const auto baseline=mode=="sparse"?evaluated():std::vector<double>();
    if(mode=="timing-review-empty") {
        popup_show(ScopeAny(global::_app->id),global::_app->popup_farkeyframes);
        QDialog* review=nullptr;for(auto widget:QApplication::topLevelWidgets())if(widget->objectName()=="studio-timing-review")review=qobject_cast<QDialog*>(widget);
        passed=passed&&review;
        if(review){auto rows=review->findChild<QListWidget*>("timing-entries");auto clean=review->findChild<QPushButton*>("timing-clean");passed=passed&&rows&&rows->count()==0&&clean&&!clean->isEnabled()&&snapshot()==original;
            QApplication::processEvents();review->grab().save(QDir::current().filePath("Data/studio-timing-review.png"));review->reject();}
        passed=passed&&snapshot()==original;reject();
    }
    else if(mode.startsWith("timing-review")) {
        const ScopeAny app(global::_app->id);history_set(app,ID_action_tl_select_single);
        RenderLlmSelectObject("floor");const IntType timeline=global::tl_edit;
        const IntType keyList=ObjType(obj_timeline,timeline)->keyframe_list;
        const IntType first=ds_list_size(keyList)>0?DsList(keyList).Value(0).ToInt():tl_keyframe_add(ScopeAny(timeline),0);
        const IntType far=tl_keyframe_add(ScopeAny(timeline),500);ObjType(obj_keyframe,far)->value=ObjType(obj_keyframe,first)->value;
        if(mode=="timing-review-lock")ObjType(obj_timeline,timeline)->lock=true;
        tl_update_length();const auto beforeReview=snapshot();const auto oldPopup=global::_app->popup;
        passed=passed&&global::_app->timeline_far_warn>0;
        popup_show(app,global::_app->popup_farkeyframes);
        QDialog* review=nullptr;for(auto widget:QApplication::topLevelWidgets())if(widget->objectName()=="studio-timing-review")review=qobject_cast<QDialog*>(widget);
        passed=passed&&review&&global::_app->popup==oldPopup;
        if(!review)error->setText("The timeline warning opened the legacy popup instead of the native timing review.");
        if(review){
            auto rows=review->findChild<QListWidget*>("timing-entries");auto clean=review->findChild<QPushButton*>("timing-clean");auto inspect=review->findChild<QPushButton*>("timing-inspect");
            passed=passed&&rows&&rows->count()==1&&clean&&(clean->isEnabled()!=(mode=="timing-review-lock"))&&inspect;
            QApplication::processEvents();review->grab().save(QDir::current().filePath("Data/studio-timing-review.png"));
            if(mode=="timing-review-clean"){
                clean->click();const auto cleaned=snapshot();passed=passed&&!review->isVisible()&&cleaned!=beforeReview&&ds_list_size(ObjType(obj_timeline,timeline)->keyframe_list)==1;
                for(const auto& track:cleaned.value("animation").toObject().value("tracks").toArray())for(const auto& key:track.toObject().value("keys").toArray())passed=passed&&key.toObject().value("frame").toInt()<500;
                mutated=cleaned!=beforeReview;QSaveFile changed(QDir::current().filePath("Data/studio-motion-changed.json"));if(changed.open(QIODevice::WriteOnly)){changed.write(QJsonDocument(cleaned).toJson());changed.commit();}
                action_toolbar_undo(app);passed=passed&&snapshot()==beforeReview;action_toolbar_redo(app);passed=passed&&snapshot()==cleaned;action_toolbar_undo(app);passed=passed&&snapshot()==beforeReview;
            } else if(mode=="timing-review-goto") {
                const auto marker=global::_app->timeline_marker;inspect->click();passed=passed&&!review->isVisible()&&global::tl_edit==timeline&&global::_app->timeline_marker==500;
                global::_app->timeline_marker=marker;app_update_animate(app);tl_update_matrix(app);passed=passed&&snapshot()==beforeReview;
            } else if(mode=="timing-review-conflict") {
                passed=renderllm_document_apply(StringType(QJsonDocument(original).toJson()))&&passed;const auto concurrent=snapshot();clean->click();
                passed=passed&&review->isVisible()&&snapshot()==concurrent;review->reject();passed=renderllm_document_undo()&&snapshot()==beforeReview&&passed;
            }
            else {review->reject();passed=passed&&snapshot()==beforeReview;}
        } else {popup_close(app);global::_app->popup=oldPopup;}
        if(mode=="timing-review-lock")ObjType(obj_timeline,timeline)->lock=false;
        action_toolbar_undo(app);passed=passed&&snapshot()==original;reject();
    }
    else if(mode=="dirty-playback") {
        passed=passed&&!renderllm_project_has_changes();const auto marker=global::_app->timeline_marker;
        RenderLlmSelectObject("sphere");const auto timeline=global::tl_edit;
        for(int at:{0,6,12,18,24}){global::_app->timeline_marker=at;tl_update_values(ScopeAny(timeline));passed=passed&&!renderllm_project_has_changes();}
        global::_app->timeline_marker=marker;tl_update_values(ScopeAny(timeline));
        auto edited=original;auto animation=edited["animation"].toObject();auto tracks=animation["tracks"].toArray();auto track=tracks[0].toObject();auto keys=track["keys"].toArray();auto key=keys[0].toObject();key["value"]=QJsonArray{0,0,10};keys[0]=key;track["keys"]=keys;tracks[0]=track;animation["tracks"]=tracks;edited["animation"]=animation;
        passed=renderllm_document_apply(StringType(QJsonDocument(edited).toJson()))&&renderllm_project_has_changes()&&passed;
        passed=renderllm_document_undo()&&!renderllm_project_has_changes()&&passed;reject();
    }
    else if(mode=="easings") {
        for(int curve=0;curve<easing->count()&&passed;++curve){
            auto base=original;auto animation=base.value("animation").toObject();auto tracks=animation.value("tracks").toArray();auto track=tracks[0].toObject();QJsonArray keys;
            const int positions[]={0,8,24,40};const double heights[]={2,8,-1,5};
            for(int i=0;i<4;++i){QJsonObject key{{"frame",positions[i]},{"value",QJsonArray{0,0,heights[i]}},{"easing",easing->itemData(curve).toString()}};if(easing->itemData(curve)=="bezier")key.insert("bezier",QJsonArray{.25,.1,.25,1});keys.append(key);}
            track.insert("keys",keys);tracks[0]=track;animation.insert("tracks",tracks);base.insert("animation",animation);
            if(!renderllm_document_apply(StringType(QJsonDocument(base).toJson(QJsonDocument::Compact)))){passed=false;break;}
            const auto expected=evaluated();auto mixed=base;
            tracks.append(QJsonObject{{"id","sparse_surface"},{"target","sphere"},{"property","material.roughness"},{"keys",QJsonArray{QJsonObject{{"frame",12},{"value",.6},{"easing","linear"}}}}});
            animation.insert("tracks",tracks);mixed.insert("animation",animation);
            if(!renderllm_document_apply(StringType(QJsonDocument(mixed).toJson(QJsonDocument::Compact)))){passed=false;renderllm_document_undo();break;}
            passed=evaluated()==expected;passed=renderllm_document_undo()&&passed;passed=renderllm_document_undo()&&snapshot()==original&&passed;
            if(!passed)error->setText("Sparse evaluation mismatch: "+easing->itemData(curve).toString());
        }
        reject();
    }
    else if(mode=="legacy-add"||mode=="legacy-history") {
        if(mode=="legacy-add")passed=renderllm_document_apply(StringType(QJsonDocument(original).toJson(QJsonDocument::Compact)));
        else history_set(ScopeAny(global::_app->id),ID_action_tl_select_single);
        RenderLlmSelectObject("floor");const IntType timeline=global::tl_edit;const IntType key=tl_keyframe_add(ScopeAny(timeline),12);ObjType(obj_keyframe,key)->value[e_value_POS_X]=3;
        const auto actual=snapshot();bool found=false;for(const auto& t:actual.value("animation").toObject().value("tracks").toArray())if(t.toObject().value("target")=="floor"&&t.toObject().value("property")=="position"){const auto keys=t.toObject().value("keys").toArray();found=keys.size()==1&&keys[0].toObject().value("frame")==12&&keys[0].toObject().value("value").toArray()[0]==3;}
        passed=passed&&found;
        if(mode=="legacy-history"){action_toolbar_undo(ScopeAny(global::_app->id));passed=passed&&snapshot()==original;action_toolbar_redo(ScopeAny(global::_app->id));passed=passed&&snapshot()==actual;action_toolbar_undo(ScopeAny(global::_app->id));passed=passed&&snapshot()==original;}
        else passed=renderllm_document_undo()&&snapshot()==original&&passed;reject();
    }
    else if(mode=="selection"){RenderLlmSelectObject("sphere");app_update_tl_edit_tabs(ScopeAny(global::_app->id));passed=passed&&!(ObjType(obj_tab,global::_app->frame_editor)->show>0)&&!(ObjType(obj_tab,global::_app->timeline_editor)->show>0)&&snapshot()==original;reject();}
    else if(mode=="compact"){resize(760,480);QApplication::processEvents();passed=passed&&height()==480&&rect().contains(apply->mapTo(this,QPoint(1,1)))&&rect().contains(apply->mapTo(this,apply->rect().bottomRight()));grab().save(QDir::current().filePath("Data/studio-motion-ui.png"));reject();}
    else if(mode=="cancel"){numbers[2]->setValue(9);reject();passed=passed&&snapshot()==original;}
    else if(mode=="noop"){apply->click();passed=passed&&!isVisible()&&snapshot()==original&&renderllm_document_revision()==revision;}
    else if(mode=="conflict"){renderllm_document_apply(StringType(QJsonDocument(original).toJson(QJsonDocument::Compact)));apply->click();passed=passed&&isVisible()&&!error->text().isEmpty()&&snapshot()==original;reject();}
    else if(mode=="collision"){frame->setValue(24);apply->click();passed=passed&&isVisible()&&!error->text().isEmpty()&&snapshot()==original;reject();}
    else if(mode=="shorten"){LoadKey(1);count->setValue(10);apply->click();passed=passed&&isVisible()&&!error->text().isEmpty()&&frame->value()==24&&snapshot()==original;reject();}
    else {
        if(mode=="value"||mode=="ui-undo")numbers[2]->setValue(9);
        else if(mode=="retime"){strip->retime(1,36);passed=Keys()[1].toObject().value("frame")==36;}
        else if(mode=="add"){frame->setValue(12);numbers[2]->setValue(4);add->click();passed=Keys().size()==3;}
        else if(mode=="remove"){remove->click();passed=Keys().size()==1;}
        else if(mode=="bezier"){easing->setCurrentIndex(easing->findData("bezier"));bezier[0]->setValue(.3);}
        else if(mode=="smooth")easing->setCurrentIndex(easing->findData("smooth"));
        else if(mode=="world"){select("$world","world.color");color->setText("#123456");add->click();passed=Keys().size()==1;}
        else if(mode=="sparse"){select("sphere","material.roughness");frame->setValue(12);numbers[0]->setValue(.6);add->click();passed=Keys().size()==1;}
        else if(mode=="timing"){fps->setValue(30);count->setValue(120);}
        else passed=false;
        if(passed){apply->click();const auto actual=snapshot();mutated=actual!=original;passed=!isVisible()&&mutated&&actual.value("scene")==original.value("scene")&&actual.value("materials")==original.value("materials");
            const auto tracks=actual.value("animation").toObject().value("tracks").toArray();QJsonArray values;for(const auto& t:tracks)if(t.toObject().value("target")==targetId&&t.toObject().value("property")==propertyId)values=t.toObject().value("keys").toArray();
            if(mode=="value")passed=passed&&values[0].toObject().value("value").toArray()[2]==9;
            if(mode=="add")passed=passed&&values.size()==3&&values[1].toObject().value("frame")==12;
            if(mode=="retime")passed=passed&&values[1].toObject().value("frame")==36;
            if(mode=="bezier")passed=passed&&values[0].toObject().value("bezier").toArray()[0]==.3;
            if(mode=="smooth")passed=passed&&values[0].toObject().value("easing")=="smooth";
            if(mode=="world")passed=passed&&values[0].toObject().value("value")=="#123456";
            if(mode=="sparse"){passed=passed&&values.size()==1&&values[0].toObject().value("frame")==12;for(const auto& t:tracks)if(t.toObject().value("id")=="bounce")passed=passed&&t==original.value("animation").toObject().value("tracks").toArray()[0];passed=passed&&evaluated()==baseline;}
            QSaveFile changed(QDir::current().filePath("Data/studio-motion-changed.json"));if(changed.open(QIODevice::WriteOnly)){changed.write(QJsonDocument(actual).toJson());changed.commit();}
            if(mode=="ui-undo") {action_toolbar_undo(ScopeAny(global::_app->id));passed=passed&&snapshot()==original;action_toolbar_redo(ScopeAny(global::_app->id));passed=passed&&snapshot()==actual;action_toolbar_undo(ScopeAny(global::_app->id));passed=passed&&snapshot()==original;}
            else passed=renderllm_document_undo()&&snapshot()==original&&passed;
        }
    }
    QSaveFile report(QDir::current().filePath("Data/studio-motion-test.json"));if(report.open(QIODevice::WriteOnly)){report.write(QJsonDocument(QJsonObject{{"mode",mode},{"passed",passed},{"mutated_then_undone",mutated},{"scene_restored",snapshot()==original},{"target",targetId},{"property",propertyId},{"key_count",Keys().size()},{"error",error->text()}}).toJson());report.commit();}
    QTimer::singleShot(0,qApp,[passed](){qApp->exit(passed?0:2);});
}
}
void Motion(){if(active){active->raise();active->activateWindow();return;}active=new MotionDialog(QApplication::activeWindow());active->show();}
}

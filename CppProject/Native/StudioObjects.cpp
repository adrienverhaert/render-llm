#include "StudioUi.hpp"
#include "StudioWidgets.hpp"
#include "RenderLlmSelection.hpp"
#include "Generated/Scripts.hpp"
#include "Generated/GmlFunc.hpp"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <functional>
#include <vector>

namespace CppProject::StudioUi {
namespace {
QJsonValue Value(const QJsonObject& object, const QString& key) {
    const auto parts=key.split('.');
    return parts.size()==1?object.value(key):object.value(parts[0]).toObject().value(parts[1]);
}
void Put(QJsonObject& object,const QString& key,const QJsonValue& value) {
    const auto parts=key.split('.');
    if(parts.size()==1){object.insert(key,value);return;}
    auto nested=object.value(parts[0]).toObject();nested.insert(parts[1],value);object.insert(parts[0],nested);
}
// The document array remains authoritative; the tree stores its stable row mapping
// without reordering objects or changing the retained timeline's parent transforms.
class SceneTree final : public QTreeWidget {
public:
    explicit SceneTree(QWidget* parent=nullptr):QTreeWidget(parent) {
        setHeaderHidden(true);setColumnCount(1);setIndentation(18);setUniformRowHeights(true);
        setSelectionMode(QAbstractItemView::ExtendedSelection);setAccessibleName("Scene hierarchy");
        setStyleSheet("QTreeWidget{background:#111113;border:0;padding:4px;}QTreeWidget::item{padding:9px 6px;}QTreeWidget::item:selected{background:#343331;color:#f3eee5;}QTreeWidget::item:hover{background:#242426;}");
    }
    int count() const {return rows.size();}
    QTreeWidgetItem* item(int row) const {return row>=0&&row<rows.size()?rows[row]:nullptr;}
    int row(QTreeWidgetItem* item) const {return rows.indexOf(item);}
    void setCurrentRow(int row) {setCurrentItem(item(row));}
    void Populate(const QJsonArray& objects) {
        clear();rows.clear();expanded.clear();filtering=false;QHash<QString,QTreeWidgetItem*> byId;
        for(const auto& entry:objects){const auto object=entry.toObject();auto item=new QTreeWidgetItem;
            item->setText(0,object.value("name").toString());item->setData(0,Qt::UserRole,object.value("id").toString());
            item->setToolTip(0,object.value("kind").toString());rows.append(item);byId.insert(object.value("id").toString(),item);}
        for(int i=0;i<objects.size();++i){const auto parent=byId.value(objects[i].toObject().value("parent_id").toString());
            if(parent)parent->addChild(rows[i]);else addTopLevelItem(rows[i]);}
        expandAll();for(auto item:rows)if(item->isExpanded())expanded.insert(item);
    }
    void Filter(const QString& text) {
        if(!filtering&&!text.isEmpty()){expanded.clear();for(auto item:rows)if(item->isExpanded())expanded.insert(item);}
        filtering=!text.isEmpty();QSet<QTreeWidgetItem*> visible;
        for(auto item:rows)if(text.isEmpty()||item->text(0).contains(text,Qt::CaseInsensitive)){
            visible.insert(item);for(auto parent=item->parent();parent;parent=parent->parent()){visible.insert(parent);if(filtering)parent->setExpanded(true);}}
        for(auto item:rows){item->setHidden(!visible.contains(item));if(!filtering)item->setExpanded(expanded.contains(item));}
    }
protected:
    void drawBranches(QPainter* painter,const QRect& rect,const QModelIndex& index) const override {
        const auto item=itemFromIndex(index);if(!item||item->childCount()==0)return;
        painter->save();painter->setRenderHint(QPainter::Antialiasing);painter->setPen(QPen(QColor("#a5a199"),1.4));
        const QPointF center(rect.right()-indentation()/2.,rect.center().y());QPolygonF mark;
        if(item->isExpanded())mark<<center+QPointF(-4,-2)<<center+QPointF(0,2)<<center+QPointF(4,-2);
        else mark<<center+QPointF(-2,-4)<<center+QPointF(2,0)<<center+QPointF(-2,4);
        painter->drawPolyline(mark);painter->restore();
    }
private:
    QList<QTreeWidgetItem*> rows;QSet<QTreeWidgetItem*> expanded;bool filtering=false;
};
class ObjectDialog final : public QDialog {
public:
    explicit ObjectDialog(QWidget* parent):QDialog(parent) {
        before=renderllm_document_snapshot().QStr();original=QJsonDocument::fromJson(before.toUtf8()).object();draft=original;
        revision=renderllm_document_revision();
        setAttribute(Qt::WA_DeleteOnClose);setWindowTitle("Objects · render-llm");resize(940,680);setStyleSheet(StyleSheet());
        auto root=new QVBoxLayout(this);root->setContentsMargins(28,24,28,24);root->setSpacing(18);
        auto heading=new QLabel("Objects");heading->setProperty("role","title");root->addWidget(heading);
        auto body=new QHBoxLayout;body->setSpacing(28);root->addLayout(body,1);
        auto rail=new QVBoxLayout;body->addLayout(rail);
        search=new QLineEdit;search->setPlaceholderText("Find an object");search->setAccessibleName("Find an object");rail->addWidget(search);
        list=new SceneTree;list->setFixedWidth(260);rail->addWidget(list,1);
        group=new QPushButton("Group selection");group->setToolTip("Wrap selected siblings in an identity group. Changes apply when you press Apply.");rail->addWidget(group);
        scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);body->addWidget(scroll,1);
        error=new QLabel;error->setWordWrap(true);error->setStyleSheet("color:#e8a399;");root->addWidget(error);
        auto buttons=new QDialogButtonBox(QDialogButtonBox::Cancel|QDialogButtonBox::Apply);root->addWidget(buttons);
        apply=buttons->button(QDialogButtonBox::Apply);apply->setDefault(true);
        connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);connect(apply,&QPushButton::clicked,this,[this](){Apply();});
        list->Populate(ObjectsArray());group->setEnabled(list->count()>0);
        connect(group,&QPushButton::clicked,this,[this](){GroupSelection();});
        connect(search,&QLineEdit::textChanged,list,&SceneTree::Filter);
        connect(list,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem* item){if(!CommitFields()){QSignalBlocker block(list);list->setCurrentRow(selected);return;}Load(list->row(item));});
        int row=0;const auto prior=RenderLlmSelectedObject();for(int i=0;i<list->count();++i)if(list->item(i)->data(0,Qt::UserRole)==prior)row=i;
        if(list->count())list->setCurrentRow(row);else {scroll->setWidget(new QLabel("Add a shape, camera or light to begin."));apply->setEnabled(false);}
        if(original.isEmpty())error->setText(renderllm_document_error().QStr());
        setWindowModality(Qt::ApplicationModal);
        if(qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_OBJECTS_UI"))QTimer::singleShot(350,this,[this](){grab().save(QDir::current().filePath("Data/studio-objects-ui.png"));});
        if(qEnvironmentVariableIsSet("RENDER_LLM_TEST_OBJECTS"))QTimer::singleShot(500,this,[this](){Test();});
    }
private:
    void GroupSelection() {
        if(!CommitFields())return;
        auto objects=ObjectsArray();const auto selection=list->selectedItems();
        if(selection.isEmpty())return;
        if(objects.size()>=1000){error->setText("The scene has reached its 1000-object limit.");return;}
        const QString parent=objects[list->row(selection.first())].toObject().value("parent_id").toString();
        for(auto item:selection)if(objects[list->row(item)].toObject().value("parent_id").toString()!=parent){error->setText("Select objects with the same parent to group them.");return;}
        const QString id="group_"+QUuid::createUuid().toString(QUuid::Id128);
        QJsonObject assembly{{"id",id},{"kind","group"},{"name","Group"},{"position",QJsonArray{0,0,0}},
            {"rotation",QJsonArray{0,0,0}},{"scale",QJsonArray{1,1,1}},{"visible",true}};
        if(!parent.isEmpty())assembly.insert("parent_id",parent);
        for(auto item:selection){const int row=list->row(item);auto child=objects[row].toObject();child.insert("parent_id",id);objects[row]=child;}
        objects.append(assembly);auto scene=draft.value("scene").toObject();scene.insert("objects",objects);draft.insert("scene",scene);
        {QSignalBlocker block(list);list->Populate(objects);list->setCurrentRow(objects.size()-1);}
        {QSignalBlocker block(search);search->clear();}
        Load(objects.size()-1);error->clear();
    }
    QJsonArray ObjectsArray() const {return draft.value("scene").toObject().value("objects").toArray();}
    bool Keyed(const QString& key) const {
        for(const auto& entry:original.value("animation").toObject().value("tracks").toArray()){
            const auto track=entry.toObject();if(track.value("target")==selectedId&&track.value("property")==key)return true;
        }return false;
    }
    void Protect(QWidget* field,const QString& key) {field->setObjectName(key);if(Keyed(key)){field->setEnabled(false);field->setToolTip("Animated in Motion; edit its keys there.");hasKeys=true;}}
    void Section(const QString& name){auto label=new QLabel(name);label->setStyleSheet("font-size:16px;font-weight:600;padding-top:12px;color:#d9d5cd;");form->addRow(label);}
    QDoubleSpinBox* Number(const QString& key,const QString& label,double low,double high,int decimals=3) {
        auto field=new QDoubleSpinBox;field->setRange(low,high);field->setDecimals(decimals);field->setButtonSymbols(QAbstractSpinBox::NoButtons);field->setAlignment(Qt::AlignRight);field->setKeyboardTracking(false);
        field->setValue(Value(model,key).toDouble());const double shown=field->value();Protect(field,key);form->addRow(label,field);
        saves.push_back([=](QJsonObject& object){if(field->isEnabled()&&field->value()!=shown)Put(object,key,field->value());});return field;
    }
    ToggleBox* Switch(const QString& key,const QString& label,const QString& trackKey={}) {
        auto field=new ToggleBox(label);field->setChecked(Value(model,key).toBool());Protect(field,trackKey.isEmpty()?key:trackKey);form->addRow(field);
        const bool shown=field->isChecked();saves.push_back([=](QJsonObject& object){if(field->isEnabled()&&field->isChecked()!=shown)Put(object,key,field->isChecked());});return field;
    }
    void Vector(const QString& key,const QString& label,double bound) {
        auto row=new QWidget;auto layout=new QHBoxLayout(row);layout->setContentsMargins(0,0,0,0);layout->setSpacing(8);
        const auto values=model.value(key).toArray();QList<QDoubleSpinBox*> fields;
        for(int axis=0;axis<3;++axis){auto field=new QDoubleSpinBox;field->setRange(-bound,bound);field->setDecimals(3);field->setSingleStep(.1);field->setButtonSymbols(QAbstractSpinBox::NoButtons);field->setValue(values[axis].toDouble());field->setProperty("shown",field->value());field->setPrefix(QString("%1  ").arg("XYZ"[axis]));field->setAccessibleName(label+" "+"XYZ"[axis]);field->setObjectName(key+QString::number(axis));layout->addWidget(field);fields.append(field);}
        Protect(row,key);form->addRow(label,row);
        saves.push_back([=](QJsonObject& object){auto updated=object.value(key).toArray();for(int axis=0;axis<3;++axis)if(row->isEnabled()&&fields[axis]->value()!=fields[axis]->property("shown").toDouble())updated[axis]=fields[axis]->value();object.insert(key,updated);});
    }
    void Color(const QString& key,const QString& label) {
        auto row=new QWidget;auto layout=new QHBoxLayout(row);layout->setContentsMargins(0,0,0,0);layout->setSpacing(12);
        auto swatch=new QPushButton;swatch->setFixedSize(30,30);swatch->setAccessibleName("Choose "+label.toLower());layout->addWidget(swatch);
        auto field=new QLineEdit(Value(model,key).toString());field->setMaxLength(7);field->setObjectName(key+".hex");field->setValidator(new QRegularExpressionValidator(QRegularExpression("#[0-9A-Fa-f]{6}"),field));layout->addWidget(field,1);colorFields.append(field);
        auto paint=[=](){if(field->hasAcceptableInput())swatch->setStyleSheet(QString("QPushButton{background:%1;border:0;border-radius:15px;}").arg(field->text()));};connect(field,&QLineEdit::textChanged,row,paint);paint();
        connect(swatch,&QPushButton::clicked,this,[guard=QPointer<QLineEdit>(field)](){if(!guard)return;QColor c(guard->text());if(!c.isValid())c=Qt::white;PickColor(c.red()|(c.green()<<8)|(c.blue()<<16),[guard](IntType rgb){if(guard)guard->setText(QColor(rgb&255,(rgb>>8)&255,(rgb>>16)&255).name().toUpper());});});
        Protect(row,key);form->addRow(label,row);const auto shown=field->text();saves.push_back([=](QJsonObject& object){if(row->isEnabled()&&field->text()!=shown)Put(object,key,field->text().toUpper());});
    }
    void Load(int row) {
        saves.clear();colorFields.clear();selected=row;hasKeys=false;share=nullptr;activeCamera=nullptr;
        if(auto old=scroll->takeWidget())old->deleteLater();
        if(row<0||row>=ObjectsArray().size())return;
        model=ObjectsArray()[row].toObject();selectedId=model.value("id").toString();
        auto content=new QWidget;form=new QFormLayout(content);form->setContentsMargins(4,0,12,12);form->setVerticalSpacing(12);form->setHorizontalSpacing(20);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);scroll->setWidget(content);
        name=new QLineEdit(model.value("name").toString());name->setMaxLength(120);form->addRow("Name",name);
        auto kind=new QLabel(model.value("kind").toString().toUpper());kind->setStyleSheet("color:#9d988f;font-size:11px;");form->addRow(kind);
        Switch("visible","Visible");Section("Transform");Vector("position","Position",1000000);Vector("rotation","Rotation · degrees",1000000);Vector("scale","Scale",10000);
        if(model.contains("parent_id")){QString parentName=model.value("parent_id").toString();for(const auto& object:ObjectsArray())if(object.toObject().value("id")==parentName)parentName=object.toObject().value("name").toString();form->addRow("Parent",new QLabel(parentName));}
        if(model.contains("material_id")) {
            const auto materialId=model.value("material_id").toString();model.insert("material",draft.value("materials").toObject().value(materialId));Section("Surface");
            Color("material.color","Color");Number("material.metallic","Metallic",0,1);Number("material.roughness","Roughness",0,1);Number("material.emission","Emission",0,1000000);
            int users=0;for(const auto& object:ObjectsArray())if(object.toObject().value("material_id")==materialId)++users;
            if(users>1){share=new ToggleBox(QString("Edit shared material (%1 objects)").arg(users));share->setToolTip("Off: changed surface values get an independent material, affecting only this object.");form->addRow(share);}
        } else if(model.contains("camera")) {
            Section("Camera");Number("camera.fov_y_deg","Field of view",1,179);Switch("camera.orthographic","Orthographic projection","camera.projection");Number("camera.ortho_height","Orthographic height",.0001,1000000,4);Switch("camera.dof","Depth of field");Number("camera.focus_distance","Focus distance",0,1000000);
            activeCamera=new ToggleBox("Use as scene camera");activeCamera->setChecked(draft.value("scene").toObject().value("active_camera_id")==selectedId);
            bool keyedCamera=false;for(const auto& entry:original.value("animation").toObject().value("tracks").toArray())if(entry.toObject().value("property")=="scene.active_camera_id")keyedCamera=true;
            activeCamera->setEnabled(!keyedCamera&&!activeCamera->isChecked());activeCamera->setToolTip("To replace this camera, select another camera and enable it there.");form->addRow(activeCamera);
        } else if(model.contains("light")) {Section("Light");Color("light.color","Color");Number("light.power","Power",0,1000000000);Number("light.size","Size",0,1000000);}
        if(hasKeys){auto hint=new QLabel("Animated values are read-only here. Their keys remain in Motion.");hint->setWordWrap(true);form->addRow(hint);}
    }
    bool CommitFields() {
        if(selected<0)return true;
        if(name->text().trimmed().isEmpty()){error->setText("Give the object a name.");name->setFocus();return false;}
        for(auto field:colorFields)if(!field->hasAcceptableInput()){error->setText("Use a six-digit color such as #FFFFFF.");field->setFocus();return false;}
        auto updated=model;updated.insert("name",name->text());for(const auto& save:saves)save(updated);
        auto materials=draft.value("materials").toObject();
        if(updated.contains("material")){
            const auto changed=updated.take("material").toObject();QString materialId=updated.value("material_id").toString();
            if(changed!=materials.value(materialId).toObject()){
                if(share&&!share->isChecked()){const auto stem=materialId.left(58)+"-local-";int suffix=1;while(materials.contains(stem+QString::number(suffix)))++suffix;materialId=stem+QString::number(suffix);updated.insert("material_id",materialId);}
                materials.insert(materialId,changed);draft.insert("materials",materials);
            }
        }
        auto objects=ObjectsArray();objects[selected]=updated;auto scene=draft.value("scene").toObject();scene.insert("objects",objects);
        if(activeCamera&&activeCamera->isEnabled()&&activeCamera->isChecked())scene.insert("active_camera_id",selectedId);
        draft.insert("scene",scene);list->item(selected)->setText(0,name->text());model=updated;
        if(updated.contains("material_id"))model.insert("material",materials.value(updated.value("material_id").toString()));
        error->clear();return true;
    }
    void Apply() {
        if(renderllm_document_revision()!=revision){error->setText("The scene changed. Close Objects and reopen it before applying.");return;}
        if(!CommitFields())return;
        if(draft!=original&&!renderllm_document_apply(StringType(QJsonDocument(draft).toJson(QJsonDocument::Compact)))){error->setText(renderllm_document_error().QStr());return;}
        RenderLlmSelectObject(selectedId);accept();
    }
    void Test();
    QString before,selectedId;QJsonObject original,draft,model;IntType revision=0;int selected=-1;bool hasKeys=false;
    SceneTree* list;QLineEdit *search,*name;QScrollArea* scroll;QFormLayout* form;QLabel* error;QPushButton *apply,*group;
    ToggleBox *share=nullptr,*activeCamera=nullptr;QList<QLineEdit*> colorFields;std::vector<std::function<void(QJsonObject&)>> saves;
};
QPointer<ObjectDialog> active;

void ObjectDialog::Test() {
    const auto mode=qEnvironmentVariable("RENDER_LLM_TEST_OBJECTS");
    auto snapshot=[](){return QJsonDocument::fromJson(renderllm_document_snapshot().ToUtf8()).object();};
    auto object=[](const QJsonObject& doc,const QString& id){for(const auto& entry:doc.value("scene").toObject().value("objects").toArray())if(entry.toObject().value("id")==id)return entry.toObject();return QJsonObject();};
    auto select=[this](const QString& id){for(int i=0;i<list->count();++i)if(list->item(i)->data(0,Qt::UserRole)==id){list->setCurrentRow(i);return true;}return false;};
    bool passed=true;bool mutated=false;
    if(mode=="empty")passed=list->count()==0&&!apply->isEnabled();
    else if(mode=="groups") {
        // Exercise the preserved editor operation before the document round trip.
        RenderLlmSelectObject("floor");
        action_tl_folder(ScopeAny(global::_app->id));
        const auto grouped=snapshot();
        const auto parentId=object(grouped,"floor").value("parent_id").toString();
        passed=!parentId.isEmpty()&&object(grouped,parentId).value("kind")=="group";
        const auto canonical=QJsonDocument(grouped).toJson(QJsonDocument::Compact);
        if(passed)passed=renderllm_document_apply(StringType(canonical))&&snapshot()==grouped;
        if(passed) {
            auto nested=grouped;auto scene=nested.value("scene").toObject();auto objects=scene.value("objects").toArray();
            QJsonObject child=object(grouped,parentId);child.insert("id","nested-group");child.insert("name","Nested assembly");child.insert("parent_id",parentId);child.insert("position",QJsonArray{2,3,4});
            objects.append(child);scene.insert("objects",objects);nested.insert("scene",scene);
            auto animation=nested.value("animation").toObject();auto tracks=animation.value("tracks").toArray();
            tracks.append(QJsonObject{{"id","group-motion"},{"target","nested-group"},{"property","position"},{"keys",QJsonArray{
                QJsonObject{{"frame",0},{"value",QJsonArray{2,3,4}},{"easing","linear"}},
                QJsonObject{{"frame",24},{"value",QJsonArray{2,3,8}},{"easing","linear"}}}}});
            animation.insert("tracks",tracks);nested.insert("animation",animation);
            passed=renderllm_document_apply(StringType(QJsonDocument(nested).toJson(QJsonDocument::Compact)));
            const auto actual=snapshot();mutated=actual!=original;
            passed=passed&&object(actual,"nested-group")==child&&object(actual,"floor").value("parent_id")==parentId;
            QSaveFile changed(QDir::current().filePath("Data/studio-objects-changed.json"));
            if(changed.open(QIODevice::WriteOnly)){changed.write(QJsonDocument(actual).toJson());changed.commit();}
            passed=renderllm_document_undo()&&snapshot()==grouped&&passed;
            const bool reapplied=renderllm_document_apply(StringType(QJsonDocument(actual).toJson(QJsonDocument::Compact)));
            passed=passed&&reapplied;
            if(reapplied&&RenderLlmSelectObject("nested-group")) {
                const IntType timeline=global::tl_edit;const auto marker=global::_app->timeline_marker;
                global::_app->timeline_marker=12;tl_update_values(ScopeAny(timeline));
                passed=passed&&ObjType(obj_timeline,timeline)->value.Value(e_value_POS_Z).ToReal()==6;
                global::_app->timeline_marker=marker;tl_update_values(ScopeAny(timeline));
            } else passed=false;
            const QString file="groups-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".renderllm";
            const auto saved=snapshot();
            const bool savedOk=renderllm_document_save(StringType(file));
            passed=passed&&savedOk;
            if(savedOk)passed=renderllm_document_open(StringType(file))&&snapshot()==saved&&passed;
            QFile::remove(QDir::current().filePath("Data/renderllm-projects/"+file));
        }
        // The first apply owns a document undo entry; the folder operation remains
        // in the original editor history. Restore through the native apply boundary.
        const bool restored=renderllm_document_apply(StringType(before))&&snapshot()==original;
        passed=passed&&restored;reject();
    }
    else if(!select("floor"))passed=false;
    else if(mode=="retired-export") {
        RenderLlmSelectObject("floor");const ScopeAny app(global::_app->id);
        const auto oldName=global::_app->context_menu_value_name;const auto oldValue=global::_app->context_menu_value;
        global::_app->context_menu_value_name="";global::_app->context_menu_value=global::tl_edit;
        const auto menu=list_init_context_menu(app,"timelinelist");
        const auto entries=ObjType(obj_list,menu)->item;bool duplicate=false,remove=false,grouped=false;
        for(IntType i=0;i<ds_list_size(entries);++i){const auto script=ObjType(obj_list_item,DsList(entries).Value(i))->script;
            if(script==ID_object_save)passed=false;duplicate|=script==ID_action_tl_duplicate;remove|=script==ID_action_tl_remove;grouped|=script==ID_action_tl_folder;}
        passed=passed&&duplicate&&remove&&grouped;
        list_destroy(app,menu);global::_app->context_menu_value_name=oldName;global::_app->context_menu_value=oldValue;
        if(passed){object_save(app);passed=global::_app->window_state==""&&snapshot()==original;}
        reject();
    }
    else if(mode=="noop") {apply->click();passed=!isVisible()&&snapshot()==original&&renderllm_document_revision()==revision;}
    else if(mode=="filter") {search->setText("softbox");int shown=0;for(int i=0;i<list->count();++i)if(!list->item(i)->isHidden())++shown;passed=shown==1&&snapshot()==original;reject();}
    else if(mode=="invalid") {name->clear();apply->click();passed=isVisible()&&!error->text().isEmpty()&&snapshot()==original;reject();}
    else if(mode=="conflict") {renderllm_document_apply(StringType(before));name->setText("Must not apply");apply->click();passed=isVisible()&&!error->text().isEmpty()&&snapshot()==original;reject();}
    else if(mode=="cancel") {name->setText("Discarded");reject();passed=snapshot()==original&&renderllm_document_revision()==revision;}
    else if(mode=="group-ui"||mode=="group-cancel"||mode=="group-filter"||mode=="group-nested") {
        QTreeWidgetItem* pedestalItem=nullptr;
        for(int i=0;i<list->count();++i)if(list->item(i)->data(0,Qt::UserRole)=="pedestal")pedestalItem=list->item(i);
        if(pedestalItem)pedestalItem->setSelected(true);else passed=false;
        group->click();QString groupId=selectedId;auto groupItem=list->currentItem();
        passed=passed&&groupItem&&groupItem->childCount()==2&&snapshot()==original;
        if(mode=="group-nested") {group->click();groupItem=list->currentItem();passed=passed&&groupItem->childCount()==1;}
        if(mode=="group-filter") {
            groupItem->setExpanded(false);search->setText("Seamless");
            int shown=0;for(int i=0;i<list->count();++i)if(!list->item(i)->isHidden())++shown;
            passed=passed&&shown==2&&groupItem->isExpanded();search->clear();
            passed=passed&&!groupItem->isExpanded();groupItem->setExpanded(true);
        }
        name->setText("Studio assembly");
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        grab().save(QDir::current().filePath("Data/studio-objects-ui.png"));
        if(mode=="group-cancel") {reject();passed=passed&&snapshot()==original&&renderllm_document_revision()==revision;}
        else {
            apply->click();const auto actual=snapshot();mutated=actual!=original;
            passed=passed&&!isVisible()&&mutated&&actual.value("animation")==original.value("animation")
                &&actual.value("materials")==original.value("materials")&&object(actual,"floor").value("parent_id")==groupId
                &&object(actual,"pedestal").value("parent_id")==groupId&&object(actual,selectedId).value("name")=="Studio assembly";
            auto floor=object(actual,"floor");floor.remove("parent_id");auto pedestal=object(actual,"pedestal");pedestal.remove("parent_id");
            passed=passed&&floor==object(original,"floor")&&pedestal==object(original,"pedestal");
            if(mode=="group-nested")passed=passed&&object(actual,groupId).value("parent_id")==selectedId;
            QSaveFile changed(QDir::current().filePath("Data/studio-objects-changed.json"));if(changed.open(QIODevice::WriteOnly)){changed.write(QJsonDocument(actual).toJson());changed.commit();}
            passed=renderllm_document_undo()&&snapshot()==original&&passed;
        }
    }
    else {
        if(mode=="animated"){passed=select("sphere");passed=passed&&!findChild<QWidget*>("position")->isEnabled();name->setText("Animated hero");}
        else if(mode=="camera") {passed=select("camera2");findChild<QDoubleSpinBox*>("camera.fov_y_deg")->setValue(45);findChild<ToggleBox*>("camera.projection")->setChecked(true);findChild<QDoubleSpinBox*>("camera.ortho_height")->setValue(12);findChild<ToggleBox*>("camera.dof")->setChecked(true);findChild<QDoubleSpinBox*>("camera.focus_distance")->setValue(7);activeCamera->setChecked(true);}
        else if(mode=="light") {passed=select("key");findChild<QDoubleSpinBox*>("light.power")->setValue(12.5);findChild<QDoubleSpinBox*>("light.size")->setValue(6);findChild<QLineEdit*>("light.color.hex")->setText("#ABCDEF");}
        else if(mode=="transform") {findChild<QDoubleSpinBox*>("position0")->setValue(2.5);findChild<QDoubleSpinBox*>("rotation2")->setValue(32);findChild<QDoubleSpinBox*>("scale0")->setValue(3);findChild<ToggleBox*>("visible")->setChecked(false);}
        else if(mode=="material"||mode=="shared") {findChild<QLineEdit*>("material.color.hex")->setText("#123456");findChild<QDoubleSpinBox*>("material.roughness")->setValue(.42);if(mode=="shared"){passed=share!=nullptr;if(share)share->setChecked(true);}}
        else if(mode=="precision")name->setText("Renamed floor");
        else if(mode=="switch") {name->setText("Renamed floor");select("key");findChild<QDoubleSpinBox*>("light.power")->setValue(12.5);select("floor");passed=name->text()=="Renamed floor";}
        else if(mode=="picker") {findChild<QWidget*>("material.color")->findChild<QPushButton*>()->click();auto picker=QApplication::activeModalWidget();if(picker&&picker!=this){picker->findChild<QLineEdit*>()->setText("#123456");picker->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();passed=snapshot()==original;}else passed=false;}
        else passed=false;
        if(passed) {
            apply->click();const auto actual=snapshot();mutated=actual!=original;
            passed=!isVisible()&&mutated&&actual.value("animation")==original.value("animation")&&RenderLlmSelectedObject()==selectedId;
            const auto floor=object(actual,"floor"),baseFloor=object(original,"floor");
            if(mode=="transform")passed=passed&&floor.value("position").toArray()[0]==2.5&&floor.value("rotation").toArray()[2]==32&&floor.value("scale").toArray()[0]==3&&!floor.value("visible").toBool();
            if(mode=="precision")passed=passed&&floor.value("position")==baseFloor.value("position")&&floor.value("scale")==baseFloor.value("scale")&&actual.value("materials")==original.value("materials");
            if(mode=="material"||mode=="shared"||mode=="picker") {
                const auto materialId=floor.value("material_id").toString();passed=passed&&actual.value("materials").toObject().value(materialId).toObject().value("color")=="#123456";
                if(mode=="shared")passed=passed&&floor.value("material_id")==object(actual,"pedestal").value("material_id");
                else passed=passed&&materialId!=baseFloor.value("material_id").toString()&&object(actual,"pedestal")==object(original,"pedestal")&&actual.value("materials").toObject().value("white")==original.value("materials").toObject().value("white");
            }
            if(mode=="camera"){const auto camera=object(actual,"camera2").value("camera").toObject();passed=passed&&camera.value("fov_y_deg")==45&&camera.value("orthographic").toBool()&&camera.value("dof").toBool()&&camera.value("focus_distance")==7&&actual.value("scene").toObject().value("active_camera_id")=="camera2";}
            if(mode=="light"||mode=="switch")passed=passed&&object(actual,"key").value("light").toObject().value("power")==12.5;
            if(mode=="light"){const auto light=object(actual,"key").value("light").toObject();passed=passed&&light.value("size")==6&&light.value("color")=="#ABCDEF";}
            if(mode=="animated")passed=passed&&object(actual,"sphere").value("name")=="Animated hero";
            QSaveFile changed(QDir::current().filePath("Data/studio-objects-changed.json"));if(changed.open(QIODevice::WriteOnly)){changed.write(QJsonDocument(actual).toJson());changed.commit();}
            passed=renderllm_document_undo()&&snapshot()==original&&passed;
        }
    }
    QSaveFile report(QDir::current().filePath("Data/studio-objects-test.json"));if(report.open(QIODevice::WriteOnly)){report.write(QJsonDocument(QJsonObject{{"passed",passed},{"mode",mode},{"mutated_then_undone",mutated},{"scene_restored",snapshot()==original}}).toJson());report.commit();}
    QTimer::singleShot(0,qApp,[passed](){qApp->exit(passed?0:2);});
}
}
void Objects(){if(active){active->raise();active->activateWindow();return;}active=new ObjectDialog(QApplication::activeWindow());active->show();}
}

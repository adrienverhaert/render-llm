#include "StudioUi.hpp"
#include "StudioWidgets.hpp"
#include "Generated/Scripts.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

namespace CppProject::StudioUi {
namespace {
QPointer<QDialog> activeLook;

QFormLayout* Section(QVBoxLayout* column, const QString& title) {
    auto heading = new QLabel(title);
    heading->setStyleSheet("color:#d9d5cd;font-size:16px;font-weight:600;margin-top:8px;");
    column->addWidget(heading);
    auto form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setVerticalSpacing(12);
    form->setHorizontalSpacing(20);
    column->addLayout(form);
    return form;
}
QDoubleSpinBox* Number(QFormLayout* form, const QString& label, double value, double low, double high, int decimals = 2) {
    auto field = new QDoubleSpinBox;
    field->setRange(low, high);
    field->setDecimals(decimals);
    field->setSingleStep(decimals ? .1 : 1);
    field->setValue(value);
    field->setProperty("initialDisplayedValue",field->value());
    field->setKeyboardTracking(false);
    field->setButtonSymbols(QAbstractSpinBox::NoButtons);
    field->setAlignment(Qt::AlignRight);
    form->addRow(label, field);
    return field;
}
QLineEdit* Color(QFormLayout* form, const QString& label, const QString& value) {
    auto row = new QWidget;
    auto layout = new QHBoxLayout(row);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(10);
    auto swatch = new QPushButton;
    swatch->setFixedSize(28,28);swatch->setToolTip("Choose "+label.toLower()+" color");
    swatch->setAccessibleName("Choose "+label.toLower()+" color");
    auto field = new QLineEdit(value.toUpper());
    field->setAccessibleName(label);
    field->setMaxLength(7);
    field->setValidator(new QRegularExpressionValidator(QRegularExpression("#[0-9A-Fa-f]{6}"), field));
    auto paint = [=]() {
        swatch->setStyleSheet(QString("QPushButton{background:%1;border-radius:14px;padding:0;}QPushButton:hover{border:2px solid #bbb7ad;}").arg(field->hasAcceptableInput()?field->text():"#555555"));
    };
    QObject::connect(field, &QLineEdit::textChanged, row, paint);
    QObject::connect(swatch,&QPushButton::clicked,row,[guard=QPointer<QLineEdit>(field)](){
        if(!guard)return;QColor color(guard->text());if(!color.isValid())color=Qt::white;
        PickColor(color.red()|(color.green()<<8)|(color.blue()<<16),[guard](IntType value){
            if(guard)guard->setText(QColor(value&255,(value>>8)&255,(value>>16)&255).name().toUpper());
        });
    });
    paint();
    layout->addWidget(swatch);
    layout->addWidget(field,1);
    form->addRow(label,row);
    return field;
}
}

void Look() {
    if(activeLook){activeLook->raise();activeLook->activateWindow();return;}
    if(qEnvironmentVariable("RENDER_LLM_TEST_LOOK")=="workspace") {
        global::_app->timeline_length=96;global::_app->timeline_marker=24;
        global::_app->timeline_zoom=global::_app->timeline_zoom_goal=12;
        global::_app->cam_work_zoom=global::_app->cam_work_zoom_goal=42;
        global::_app->cam_work_roll=17;
        global::_app->cam_work_focus=point3D(2,3,4);
        camera_work_set_from(ScopeAny(global::_app->id));
    }
    if(qEnvironmentVariable("RENDER_LLM_TEST_LOOK")=="precision") {
        global::_app->background_sunlight_strength=.123456789;
        global::_app->background_sunlight_angle=.5260123;
        global::_app->project_cinema_noise=.0123456789;
    }
    const QString before = renderllm_document_snapshot().QStr();
    const auto original = QJsonDocument::fromJson(before.toUtf8()).object();
    if(original.isEmpty()) {Message("Settings unavailable", renderllm_document_error().QStr(), {"Close"},0);return;}
    const IntType revision = renderllm_document_revision();
    const auto world = original.value("scene").toObject().value("world").toObject();
    const auto render = original.value("render").toObject();
    auto dialog = new QDialog(QApplication::activeWindow());
    activeLook = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Lighting & render · render-llm");
    dialog->setStyleSheet(StyleSheet());
    dialog->resize(880,640);
    auto root = new QVBoxLayout(dialog);
    root->setContentsMargins(28,24,28,24);
    root->setSpacing(18);
    auto title = new QLabel("Shape the light");
    title->setProperty("role","title");
    root->addWidget(title);
    auto note = new QLabel("Set the atmosphere and renderer. Changes apply together as one undoable edit.");
    note->setWordWrap(true);
    root->addWidget(note);
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea,QScrollArea>QWidget>QWidget{background:#171719;}");
    auto content = new QWidget;
    auto columns = new QHBoxLayout(content);
    columns->setContentsMargins(0,0,12,0);
    columns->setSpacing(36);
    auto left = new QVBoxLayout;
    auto right = new QVBoxLayout;
    columns->addLayout(left,1);columns->addLayout(right,1);
    scroll->setWidget(content);root->addWidget(scroll,1);
    QHash<QString,QLineEdit*> colors;
    QHash<QString,QDoubleSpinBox*> numbers;
    auto light = Section(left,"Studio lighting");
    const QPair<const char*,const char*> lightColors[] = {{"color","Backdrop"},{"ambient_color","Fill light"},{"sun_color","Key light"}};
    for(const auto& item : lightColors)
        colors.insert(item.first,Color(light,item.second,world.value(item.first).toString("#FFFFFF")));
    numbers.insert("strength",Number(light,"Key strength",world.value("strength").toDouble(),0,1000000,3));
    numbers.insert("elevation",Number(light,"Key tilt",world.value("elevation").toDouble(),-1000000,1000000,2));
    numbers["elevation"]->setSuffix("°");
    numbers["elevation"]->setToolTip("0° is overhead; 90° is on the horizon.");
    numbers.insert("rotation",Number(light,"Key rotation",world.value("rotation").toDouble(),-1000000,1000000,2));
    numbers["rotation"]->setSuffix("°");
    numbers.insert("angular_size",Number(light,"Light spread",world.value("angular_size").toDouble(),0,1000000,3));
    numbers["angular_size"]->setSuffix("°");
    auto atmosphere = Section(left,"Atmosphere");
    auto fog = new ToggleBox("Enable haze");fog->setChecked(world.value("fog_enabled").toBool());
    atmosphere->addRow(fog);
    auto hazeFields = new QWidget;
    auto hazeForm = new QFormLayout(hazeFields);hazeForm->setContentsMargins(0,0,0,0);hazeForm->setVerticalSpacing(12);
    hazeForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    left->addWidget(hazeFields);hazeFields->setVisible(fog->isChecked());
    QObject::connect(fog,&QCheckBox::toggled,hazeFields,&QWidget::setVisible);
    colors.insert("fog_color",Color(hazeForm,"Haze color",world.value("fog_color").toString("#FFFFFF")));
    numbers.insert("fog_distance",Number(hazeForm,"Distance",world.value("fog_distance").toDouble(),0,1000000));
    numbers.insert("fog_height",Number(hazeForm,"Height",world.value("fog_height").toDouble(),0,1000000));
    auto quality = Section(right,"Renderer");
    auto engine = new QComboBox;
    engine->addItems({"Flame","UltraFlame"});engine->setCurrentText(render.value("renderer").toString());
    quality->addRow("Engine",engine);
    auto exposure = Number(quality,"Exposure",render.value("exposure").toDouble(1),0,10000,3);
    auto bounces = Number(quality,"Light bounces",render.value("bounces").toInt(4),0,64,0);
    auto noise = Number(quality,"Noise threshold",render.value("noise_threshold").toDouble(),0,1,4);
    auto denoise = new ToggleBox("Denoise the final image");denoise->setChecked(render.value("denoise").toBool(true));
    quality->addRow(denoise);
    auto engineNote = new QLabel;engineNote->setWordWrap(true);right->addWidget(engineNote);
    auto selectEngine = [=]() {
        const bool ultra = engine->currentText()=="UltraFlame";
        bounces->setEnabled(ultra);noise->setEnabled(ultra);denoise->setEnabled(ultra);
        engineNote->setText(ultra?"Path-traced lighting. Backend and denoiser are confirmed when rendering completes.":"Real-time raster lighting. Path-tracing controls do not apply to Flame.");
    };
    QObject::connect(engine,qOverload<int>(&QComboBox::currentIndexChanged),dialog,selectEngine);selectEngine();
    auto outputNote = new QLabel("Choose resolution, samples and duration in Output.");outputNote->setWordWrap(true);right->addWidget(outputNote);
    // Static lighting controls must not silently override or appear to edit keyed values.
    bool animatedLighting = false;
    for(const auto& value : original.value("animation").toObject().value("tracks").toArray()) {
        const auto track = value.toObject();
        if(track.value("target")!="$world")continue;
        if(track.value("property")=="world.color"){colors["color"]->parentWidget()->setEnabled(false);animatedLighting=true;}
        if(track.value("property")=="world.strength"){numbers["strength"]->setEnabled(false);animatedLighting=true;}
    }
    if(animatedLighting){auto hint=new QLabel("Animated lighting values follow their Motion keys and are read-only here.");hint->setWordWrap(true);right->addWidget(hint);}
    left->addStretch();right->addStretch();
    auto error = new QLabel;error->setWordWrap(true);error->setStyleSheet("color:#e8a399;");root->addWidget(error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel|QDialogButtonBox::Apply);
    root->addWidget(buttons);buttons->button(QDialogButtonBox::Apply)->setDefault(true);
    QObject::connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
    QObject::connect(buttons->button(QDialogButtonBox::Apply),&QPushButton::clicked,dialog,[=]() {
        if(renderllm_document_revision()!=revision){error->setText("The scene changed. Close these settings and reopen them before applying.");return;}
        auto updatedWorld = world;
        for(auto it=colors.begin();it!=colors.end();++it){
            if(!it.value()->hasAcceptableInput()){error->setText("Use a six-digit color such as #FFFFFF.");it.value()->setFocus();return;}
            updatedWorld.insert(it.key(),it.value()->text().toUpper());
        }
        for(auto it=numbers.begin();it!=numbers.end();++it)
            if(it.value()->value()!=it.value()->property("initialDisplayedValue").toDouble())updatedWorld.insert(it.key(),it.value()->value());
        updatedWorld.insert("fog_enabled",fog->isChecked());
        auto updatedRender = render;
        updatedRender.insert("schema",2);updatedRender.insert("renderer",engine->currentText());
        if(exposure->value()!=exposure->property("initialDisplayedValue").toDouble())updatedRender.insert("exposure",exposure->value());
        updatedRender.insert("bounces",int(bounces->value()));
        if(noise->value()!=noise->property("initialDisplayedValue").toDouble())updatedRender.insert("noise_threshold",noise->value());
        updatedRender.insert("denoise",denoise->isChecked());
        auto document = original;auto scene = document.value("scene").toObject();scene.insert("world",updatedWorld);
        document.insert("scene",scene);document.insert("render",updatedRender);
        if(document==original){dialog->accept();return;}
        if(!renderllm_document_apply(StringType(QJsonDocument(document).toJson(QJsonDocument::Compact)))){error->setText(renderllm_document_error().QStr());return;}
        dialog->accept();
    });
    dialog->setWindowModality(Qt::ApplicationModal);dialog->show();
    if(qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_LOOK_UI"))QTimer::singleShot(350,dialog,[=](){dialog->grab().save(QDir::current().filePath("Data/studio-look-ui.png"));});
    if(qEnvironmentVariableIsSet("RENDER_LLM_TEST_LOOK"))QTimer::singleShot(500,dialog,[=]() {
        const QString mode=qEnvironmentVariable("RENDER_LLM_TEST_LOOK");
        if(mode=="noop") {
            buttons->button(QDialogButtonBox::Apply)->click();
            const bool passed=!dialog->isVisible()&&renderllm_document_revision()==revision&&renderllm_document_snapshot().QStr()==before;
            QTimer::singleShot(0,qApp,[passed](){qApp->exit(passed?0:2);});return;
        }
        if(colors["color"]->isEnabled()&&mode!="picker")colors["color"]->setText("#123456");
        engine->setCurrentText("Flame");exposure->setValue(1.75);
        fog->click();
        bool passed=HasModal();
        if(mode=="picker") {
            colors["color"]->parentWidget()->findChild<QPushButton*>()->click();
            auto picker=QApplication::activeModalWidget();
            if(!picker||picker==dialog){qApp->exit(2);return;}
            picker->findChild<QLineEdit*>()->setText("#123456");
            picker->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            passed=passed&&colors["color"]->text()=="#123456"&&renderllm_document_snapshot().QStr()==before;
        }
        if(mode=="conflict") {
            passed=passed&&renderllm_document_apply(StringType(before));
            const auto concurrent=renderllm_document_snapshot();
            buttons->button(QDialogButtonBox::Apply)->click();
            passed=passed&&dialog->isVisible()&&!error->text().isEmpty()&&renderllm_document_snapshot()==concurrent;
            dialog->reject();
        } else if(mode=="invalid") {
            colors["color"]->setText("#12");buttons->button(QDialogButtonBox::Apply)->click();
            passed=passed&&dialog->isVisible()&&!error->text().isEmpty()&&renderllm_document_snapshot().QStr()==before;
            dialog->reject();
        } else if(mode=="cancel") {
            buttons->button(QDialogButtonBox::Cancel)->click();
            passed=passed&&renderllm_document_snapshot().QStr()==before;
        } else {
            buttons->button(QDialogButtonBox::Apply)->click();
            const auto actual=QJsonDocument::fromJson(renderllm_document_snapshot().ToUtf8()).object();
            passed=passed&&!dialog->isVisible()&&actual.value("render").toObject().value("renderer")=="Flame"
                &&actual.value("render").toObject().value("exposure").toDouble()==1.75
                &&actual.value("scene").toObject().value("world").toObject().value("color")==
                    (mode=="animated"?world.value("color"):QJsonValue("#123456"));
            passed=passed&&actual.value("animation")==original.value("animation");
            if(mode=="precision")passed=passed&&actual.value("scene").toObject().value("world").toObject().value("strength")==world.value("strength")
                &&actual.value("scene").toObject().value("world").toObject().value("angular_size")==world.value("angular_size")
                &&actual.value("render").toObject().value("noise_threshold")==render.value("noise_threshold");
            if(mode=="animated")passed=passed&&!colors["color"]->isEnabled();
            passed=passed&&actual.value("scene").toObject().value("world").toObject().value("fog_enabled").toBool()!=world.value("fog_enabled").toBool();
            passed=passed&&renderllm_document_undo()&&renderllm_document_snapshot().QStr()==before;
            if(mode=="workspace")passed=passed&&global::_app->timeline_marker==24&&global::_app->timeline_zoom==12
                &&global::_app->cam_work_zoom==42&&global::_app->cam_work_roll==17
                &&global::_app->cam_work_focus.Value(X_)==2&&global::_app->cam_work_focus.Value(Y_)==3&&global::_app->cam_work_focus.Value(Z_)==4;
        }
        QTimer::singleShot(0,qApp,[passed](){qApp->exit(passed?0:2);});
    });
}
}

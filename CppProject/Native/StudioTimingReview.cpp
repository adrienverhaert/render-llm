#include "StudioUi.hpp"
#include "Generated/Scripts.hpp"
#include <QApplication>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>
#include <vector>

namespace CppProject::StudioUi {
namespace {
struct TimingEntry { IntType timeline; double frame; int index, channels; bool locked; QString name; };
std::vector<TimingEntry> ReadEntries() {
    std::vector<TimingEntry> entries;
    const auto list=mi3_tl_far_list();
    for(IntType i=0;i<ds_list_size(list);++i) {
        const ArrType row=DsList(list).Value(i);const IntType timeline=row.Value(0).ToInt();
        entries.push_back({timeline,row.Value(2).ToReal(),int(row.Value(1).ToInt()),int(row.Value(3).ToInt()),
            ObjType(obj_timeline,timeline)->lock>0,mi3_tl_far_path(timeline).ToStr().QStr()});
    }
    ds_list_destroy(list);return entries;
}
class TimingReviewDialog final : public QDialog {
public:
    explicit TimingReviewDialog(QWidget* parent):QDialog(parent) {
        setObjectName("studio-timing-review");setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle("Timing review · render-llm");resize(680,460);setWindowModality(Qt::ApplicationModal);
        // Opening a review is read-only. Cleanup itself owns length recomputation.
        if(global::_app->timeline_far_warn>0)entries=ReadEntries();before=renderllm_document_snapshot().QStr();
        auto root=new QVBoxLayout(this);root->setContentsMargins(28,24,28,24);root->setSpacing(16);
        auto title=new QLabel("Review the timeline tail");title->setProperty("role","title");root->addWidget(title);
        auto note=new QLabel("These keys follow a long gap. Inspect a key or remove only unchanged, unlocked keys. Undo remains available.");note->setWordWrap(true);root->addWidget(note);
        list=new QListWidget;list->setObjectName("timing-entries");list->setSpacing(4);
        list->setStyleSheet("QListWidget::item{padding:8px;border:0;}QListWidget::item:selected{background:#343331;color:#f3eee5;}QListWidget::item:hover{background:#29282a;}");root->addWidget(list,1);
        int removable=0;
        for(const auto& entry:entries){
            const bool noop=entry.index>0&&entry.channels==0&&!entry.locked;removable+=noop;
            const QString detail=entry.locked?"Locked":entry.index==0?"First key · retained":noop?"Unchanged key":QString("%1 changed channels").arg(entry.channels);
            auto item=new QListWidgetItem(QString("%1\nFrame %2 · %3").arg(entry.name).arg(entry.frame,0,'g',10).arg(detail),list);item->setSizeHint(QSize(0,58));
        }
        if(!entries.empty())list->setCurrentRow(0);
        status=new QLabel(entries.empty()?"No distant keys to review.":QString("%1 %2 · %3 unchanged and unlocked").arg(entries.size()).arg(entries.size()==1?"key":"keys").arg(removable));status->setWordWrap(true);root->addWidget(status);
        auto actions=new QHBoxLayout;root->addLayout(actions);
        auto clean=new QPushButton("Remove unchanged keys");clean->setObjectName("timing-clean");clean->setEnabled(removable>0);actions->addWidget(clean);actions->addStretch();
        auto close=new QPushButton("Close");close->setDefault(true);actions->addWidget(close);
        auto inspect=new QPushButton("Go to key");inspect->setObjectName("timing-inspect");inspect->setEnabled(!entries.empty());actions->addWidget(inspect);
        connect(close,&QPushButton::clicked,this,&QDialog::reject);
        connect(inspect,&QPushButton::clicked,this,[this](){Inspect();});
        connect(list,&QListWidget::itemDoubleClicked,this,[this](){Inspect();});
        connect(clean,&QPushButton::clicked,this,[this](){
            if(!Current())return;
            action_tl_far_clean(ScopeAny(global::_app->id));accept();
        });
    }
private:
    bool Current() {
        // A modal can outlive a programmatic edit. Never apply stored native IDs
        // to a different document, or treat a newly locked key as removable.
        if(renderllm_document_snapshot().QStr()!=before){status->setText("The scene changed. Close and reopen this review.");return false;}
        const auto now=ReadEntries();bool same=now.size()==entries.size();
        for(size_t i=0;same&&i<now.size();++i)same=now[i].timeline==entries[i].timeline&&now[i].frame==entries[i].frame&&now[i].index==entries[i].index&&now[i].channels==entries[i].channels&&now[i].locked==entries[i].locked;
        if(!same)status->setText("The keys changed. Close and reopen this review.");return same;
    }
    void Inspect() {
        const int index=list->currentRow();if(index<0||index>=int(entries.size())||!Current())return;
        const auto entry=entries[size_t(index)];accept();
        action_tl_far_goto(ScopeAny(global::_app->id),entry.timeline,entry.frame);
    }
    QString before;std::vector<TimingEntry> entries;QListWidget* list;QLabel* status;
};
QPointer<TimingReviewDialog> timingReview;
}
void ReviewTiming(){if(timingReview){timingReview->raise();timingReview->activateWindow();return;}timingReview=new TimingReviewDialog(QApplication::activeWindow());timingReview->show();}
}

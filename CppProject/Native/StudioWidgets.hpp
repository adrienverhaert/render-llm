#pragma once
#include <QCheckBox>
#include <QPainter>

namespace CppProject::StudioUi {
// Keep Qt's keyboard, focus and accessibility behavior; replace only presentation.
class ToggleBox final : public QCheckBox {
public:
    explicit ToggleBox(const QString& text, QWidget* parent = nullptr) : QCheckBox(text,parent) {setMinimumHeight(26);}
    QSize sizeHint() const override {return {fontMetrics().horizontalAdvance(text())+42,qMax(26,fontMetrics().height()+6)};}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.setOpacity(isEnabled()?1.:.4);
        const int y=(height()-16)/2;
        p.setPen(Qt::NoPen);p.setBrush(isChecked()?QColor("#c9c4bb"):QColor("#484646"));
        p.drawRoundedRect(QRectF(0,y,28,16),8,8);
        p.setBrush(isChecked()?QColor("#242424"):QColor("#bbb6ad"));
        p.drawEllipse(QRectF(isChecked()?14:2,y+2,12,12));
        p.setPen(QColor("#ddd9d2"));p.drawText(rect().adjusted(38,0,0,0),Qt::AlignVCenter|Qt::AlignLeft,text());
        if(hasFocus()){p.setPen(QPen(QColor("#bbb7ad"),1,Qt::DashLine));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(rect()).adjusted(1,1,-1,-1),4,4);}
    }
    bool hitButton(const QPoint& point) const override {return rect().contains(point);}
};
}

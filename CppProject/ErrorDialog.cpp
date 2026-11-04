#include "ErrorDialog.hpp"
#include "Native/StudioUi.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace CppProject
{
    ErrorDialog::ErrorDialog(QString error)
    {
        // May run before generated globals/fonts are ready. Never upload diagnostics
        // or restart implicitly; this dialog is independent of the scene runtime.
        setWindowTitle("Editor interrupted · render-llm");
        setWindowFlag(Qt::WindowContextHelpButtonHint, false);
        setStyleSheet(StudioUi::StyleSheet());
        setMinimumWidth(560);
        auto layout = new QVBoxLayout(this);
        layout->setContentsMargins(32, 28, 32, 28);
        layout->setSpacing(20);
        auto title = new QLabel("The editor had to stop");
        title->setProperty("role", "title");
        layout->addWidget(title);
        auto explanation = new QLabel("An internal error interrupted this session. Unsaved changes may be lost.\nClose the app before opening it again.");
        explanation->setWordWrap(true);
        layout->addWidget(explanation);
        const QString diagnostics = QStringLiteral("render-llm\nApplication: %1\n\n%2")
            .arg(QCoreApplication::applicationFilePath(), error);
        auto details = new QTextEdit;
        details->setReadOnly(true);
        details->setPlainText(diagnostics);
        details->setMinimumHeight(200);
        details->hide();
        auto disclosure = new QToolButton;
        disclosure->setText("Technical details");
        disclosure->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        disclosure->setArrowType(Qt::RightArrow);
        disclosure->setCheckable(true);
        disclosure->setStyleSheet("QToolButton{color:#bdb9b1;background:transparent;border:0;padding:4px 0;}");
        connect(disclosure, &QToolButton::toggled, this, [=](bool show) {
            details->setVisible(show);
            disclosure->setArrowType(show ? Qt::DownArrow : Qt::RightArrow);
            adjustSize();
        });
        layout->addWidget(disclosure);
        layout->addWidget(details);
        auto privacy = new QLabel("Diagnostics stay on this computer. Nothing is uploaded.");
        privacy->setWordWrap(true);
        layout->addWidget(privacy);
        auto actions = new QHBoxLayout;
        actions->setSpacing(10);
        auto copy = new QPushButton("Copy details");
        connect(copy, &QPushButton::clicked, this, [diagnostics, copy]() {
            QApplication::clipboard()->setText(diagnostics);
            copy->setText("Copied");
        });
        const QString logPath = QDir::current().absoluteFilePath("Data/log.txt");
        auto log = new QPushButton("Open local log");
        log->setEnabled(QFileInfo(logPath).isFile());
        connect(log, &QPushButton::clicked, this, [logPath]() {
            if (QFileInfo(logPath).isFile()) QDesktopServices::openUrl(QUrl::fromLocalFile(logPath));
        });
        auto close = new QPushButton("Close app");
        close->setDefault(true);
        connect(close, &QPushButton::clicked, this, &QDialog::accept);
        actions->addWidget(copy);
        actions->addWidget(log);
        actions->addStretch();
        actions->addWidget(close);
        layout->addLayout(actions);
        if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_ERROR_UI")) {
            QTimer::singleShot(400, this, [=]() {
                disclosure->click();
                const bool contract = details->isVisible() && details->toPlainText().contains(error)
                    && !details->toPlainText().contains("Mine-imator");
                const bool saved = grab().save(QDir::current().filePath("Data/studio-error-ui.png"));
                close->click();
                QTimer::singleShot(0, qApp, [=]() { qApp->exit(contract && saved ? 0 : 2); });
            });
        }
        exec();
    }
}

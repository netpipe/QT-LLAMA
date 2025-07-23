#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QFileDialog>
#include <QLabel>

class LlamaFrontend : public QWidget {
    Q_OBJECT

public:
    LlamaFrontend(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);

        modelLabel = new QLabel("Model: (Click to choose)", this);
        modelLabel->setStyleSheet("color: blue; text-decoration: underline;");
        layout->addWidget(modelLabel);
        connect(modelLabel, &QLabel::linkActivated, this, &LlamaFrontend::chooseModel);
        modelLabel->installEventFilter(this);

        input = new QLineEdit(this);
        input->setPlaceholderText("Enter your prompt...");
        layout->addWidget(input);

        auto *button = new QPushButton("Run llama-cli", this);
        layout->addWidget(button);

        output = new QPlainTextEdit(this);
        output->setReadOnly(true);
        layout->addWidget(output);

        connect(button, &QPushButton::clicked, this, &LlamaFrontend::runLlama);

        proc = new QProcess(this);
        connect(proc, &QProcess::readyReadStandardOutput, this, [=]() {
            output->appendPlainText(proc->readAllStandardOutput());
        });

        connect(proc, &QProcess::readyReadStandardError, this, [=]() {
            output->appendPlainText(proc->readAllStandardError());
        });
    }

protected:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (obj == modelLabel && event->type() == QEvent::MouseButtonRelease) {
            chooseModel();
            return true;
        }
        return QWidget::eventFilter(obj, event);
    }

private:
    QLineEdit *input;
    QPlainTextEdit *output;
    QProcess *proc;
    QLabel *modelLabel;
    QString modelPath = "./model.gguf";  // Default model path

    void chooseModel() {
        QString file = QFileDialog::getOpenFileName(this, "Choose Model", "", "GGUF Model (*.gguf)");
        if (!file.isEmpty()) {
            modelPath = file;
            modelLabel->setText("Model: " + modelPath);
        }
    }

    void runLlama() {
        QString prompt = input->text().trimmed();
        if (prompt.isEmpty()) return;

        output->clear();
        QStringList args;
        args << "--flash-attn"
             << "--model" << modelPath
             << "--prompt" << prompt
             << "--n-predict" << "64";  // You can expose this as UI too

        proc->start("llama-cli", args);
    }
};

#include "main.moc"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    LlamaFrontend window;
    window.setWindowTitle("LLaMA CLI Frontend");
    window.resize(600, 400);
    window.show();
    return app.exec();
}

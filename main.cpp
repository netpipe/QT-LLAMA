#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QFileDialog>
#include <QLabel>
#include <QDebug>
#include <QProcessEnvironment>
#include <QByteArray>
#include <QProcess>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

class LlamaFrontend : public QWidget {
    Q_OBJECT

public:
    LlamaFrontend(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);

        modelLabel = new QLabel("Model: (Click to choose)", this);
        modelLabel->setStyleSheet("color: blue; text-decoration: underline;");

        layout->addWidget(modelLabel);

tokens = new QLineEdit(this);
tokens->setText("160");
// layout->addWidget(ltokens);
    layout->addWidget(tokens);

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
            QByteArray data = proc->readAllStandardOutput();
            output->moveCursor(QTextCursor::End);
            output->insertPlainText(QString::fromUtf8(data));
        });

QPushButton *downloadModelBtn = new QPushButton("download", this);
layout->addWidget(downloadModelBtn);
        manager = new QNetworkAccessManager(this);
        QObject::connect(downloadModelBtn, &QPushButton::clicked, [&]() {
            if (!QFileInfo::exists("tinyllama-1.1b-chat-v1.0.Q6_K.gguf")){
            QString initialUrl = "https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q6_K.gguf";
           // QString outputDir = "/model";
            QString outputFile = QApplication::applicationDirPath() + "/tinyllama-1.1b-chat-v1.0.Q6_K.gguf";

        //    QDir().mkpath(outputDir);
            QNetworkRequest request(initialUrl);
            QNetworkReply *reply = manager->get(request);

            QObject::connect(reply, &QNetworkReply::finished, [=]() {
                QVariant redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
                if (redirect.isValid()) {
                    // Got a redirect (LFS behavior), follow it
                    QUrl redirectedUrl = redirect.toUrl();
                    QNetworkRequest newReq(redirectedUrl);
                    QNetworkReply *finalReply = manager->get(newReq);

                    QObject::connect(finalReply, &QNetworkReply::finished, [=]() {
                        if (finalReply->error() == QNetworkReply::NoError) {
                            QFile file(outputFile);
                            if (file.open(QIODevice::WriteOnly)) {
                                file.write(finalReply->readAll());
                                file.close();
                                input->setText("✅ Model downloaded after redirect!");
                            } else {
                                input->setText("❌ Failed to save redirected model.");
                            }
                        } else {
                            input->setText("❌ Redirected download failed: " + finalReply->errorString());
                        }
                        finalReply->deleteLater();
                    });
                } else {
                    // No redirect (unlikely for Hugging Face LFS)
                    if (reply->error() == QNetworkReply::NoError) {
                        QFile file(outputFile);
                        if (file.open(QIODevice::WriteOnly)) {
                            file.write(reply->readAll());
                            file.close();
                            input->setText("✅ Model downloaded directly!");
                        } else {
                            input->setText("❌ Failed to write model file.");
                        }
                    } else {
                        input->setText("❌ Download failed: " + reply->errorString());
                    }
                }
                reply->deleteLater();
            });

            input->setText("📥 Downloading model could take a while!(1.5GB)...");
            }else{  input->setText("❌ already downloaded.");}
        });

        connect(proc, &QProcess::readyReadStandardError, this, [=]() {
            QByteArray data = proc->readAllStandardError();
            output->appendPlainText(QString::fromLocal8Bit(data));
            qDebug() << "STDERR:" << data;
        });

        connect(proc, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this, [=](int code, QProcess::ExitStatus status) {
            qDebug() << "Process finished with code:" << code << "status:" << status;
            if (status == QProcess::CrashExit)
                output->appendPlainText("llama-cli crashed.");
        });

        connect(proc, &QProcess::errorOccurred, this, [=](QProcess::ProcessError error) {
            qDebug() << "Process error:" << error;
            output->appendPlainText("Process error occurred. Check if llama-cli is in your PATH.");
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
    QString modelPath = QApplication::applicationDirPath() + "/tinyllama-1.1b-chat-v1.0.Q6_K.gguf";  // Default
    QLabel *ltokens;
    QLineEdit *tokens;
QNetworkAccessManager *manager;
    void chooseModel() {
        QString file = QFileDialog::getOpenFileName(this, "Choose Model", "", "GGUF Model (*.gguf)");
        if (!file.isEmpty()) {
            modelPath = file;
            modelLabel->setText("Model: " + modelPath);
        }
    }

    void runLlama() {

            if (proc->state() != QProcess::NotRunning) {
                proc->kill();  // Immediately kills the process
                proc->waitForFinished();  // Optional: wait until it's dead
                qDebug() << "Process was killed.";
            }


        QString prompt = input->text().trimmed();
        if (prompt.isEmpty()) {
            output->appendPlainText("Prompt is empty.");
            return;
        }

        if (!QFile::exists(modelPath)) {
            output->appendPlainText("Model file does not exist: " + modelPath);
            return;
        }

        output->clear();
        output->appendPlainText("Running llama-cli...\n");

        QStringList args;
        args << "--flash-attn"
             << "--model" << modelPath
             << "--prompt" << prompt
             << "--n-predict" << tokens->text();

        qputenv("DYLD_LIBRARY_PATH",  QApplication::applicationDirPath().toUtf8()
+ "/");
    //    qputenv("DYLD_LIBRARY_PATH", "/Applications/QT-LLAMA.app/Contents/MacOS");

        QString command = QApplication::applicationDirPath() + "/llama-cli";
        output->appendPlainText("Command: " + command + " " + args.join(" "));

        qDebug() << "Starting process:" << command << args;
        proc->start(command, args);
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

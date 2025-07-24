#include <QApplication>
#include <QWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QLineEdit>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QFile>
#include <QFileDialog>

class LlamaClient : public QWidget {
    Q_OBJECT

public:
    LlamaClient(QWidget *parent = nullptr) : QWidget(parent) {
        setWindowTitle("LLaMA Server/Client");

        auto *layout = new QVBoxLayout(this);
        output = new QTextEdit(this);
        output->setReadOnly(true);
        input = new QLineEdit(this);
        QPushButton *askBtn = new QPushButton("Ask", this);
        QPushButton *modelbtn = new QPushButton("OpenModel", this);
        QPushButton *startServerBtn = new QPushButton("Start Server", this);
        tokeninput = new QLineEdit(this);
        tokeninput->setText("128");

        connect(modelbtn, &QPushButton::clicked, this, &LlamaClient::modelopen);
 layout->addWidget(tokeninput);
        layout->addWidget(new QLabel("Ask LLaMA:"));
        layout->addWidget(input);
        layout->addWidget(askBtn);
        layout->addWidget(new QLabel("Response:"));
        layout->addWidget(output);
        layout->addWidget(startServerBtn);


        QPushButton *downloadModelBtn = new QPushButton("download", this);
        layout->addWidget(downloadModelBtn);
                manager = new QNetworkAccessManager(this);
                QObject::connect(downloadModelBtn, &QPushButton::clicked, [&]() {
                    if (!QFileInfo::exists(QApplication::applicationDirPath() + "/tinyllama-1.1b-chat-v1.0.Q6_K.gguf")){
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


        connect(askBtn, &QPushButton::clicked, this, &LlamaClient::sendPrompt);
        connect(startServerBtn, &QPushButton::clicked, this, &LlamaClient::startServer);

        manager = new QNetworkAccessManager(this);
        serverProcess = new QProcess(this);
    }

private slots:
    void sendPrompt() {
        QString prompt = input->text().trimmed();
        if (prompt.isEmpty()) return;

        QUrl url("http://localhost:8080/completion");
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

        QJsonObject body;
        body["prompt"] = prompt;
        nTokens = tokeninput->text().toInt();
        body["n_predict"] = nTokens;

        QJsonDocument doc(body);
        QByteArray data = doc.toJson();

        QNetworkReply *reply = manager->post(request, data);
        connect(reply, &QNetworkReply::finished, [this, reply]() {
            QByteArray response = reply->readAll();
            reply->deleteLater();

            QJsonDocument json = QJsonDocument::fromJson(response);
            if (json.isObject() && json.object().contains("content")) {
                output->append("> " + input->text());
                output->append(json.object()["content"].toString() + "\n");
            } else {
                output->append("[Error] Unexpected response.");
            }
        });
    }


    void modelopen() {
        QString file = QFileDialog::getOpenFileName(this, "Choose Model", "", "GGUF Model (*.gguf)");
        if (!file.isEmpty()) {
            modelPath = file;
            //modelLabel->setText("Model: " + modelPath);
        }

    }

    void startServer() {
        if (serverProcess->state() == QProcess::Running) {
            output->append("[Server already running]");
            return;
        }
        qputenv("DYLD_LIBRARY_PATH",  QApplication::applicationDirPath().toUtf8()
+ "/");
        QString program = QApplication::applicationDirPath() + "/llama-server";  // Adjust to path of your llama server binary
        QStringList arguments;
        arguments << "--model" << QApplication::applicationDirPath() + modelPath << "--port" << "8080";

        serverProcess->start(program, arguments);
        if (!serverProcess->waitForStarted(3000)) {
            output->append("[Failed to start server]");
        } else {
            output->append("[Server started]");
        }
    }

private:
    QTextEdit *output;
    QLineEdit *input;
    QNetworkAccessManager *manager;
    QNetworkAccessManager *manager2;
    QProcess *serverProcess;
    QString modelPath =  "/tinyllama-1.1b-chat-v1.0.Q6_K.gguf";
    int nTokens=128;
    QLineEdit *tokeninput;
};

#include "main.moc"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    LlamaClient window;
    window.resize(600, 400);
    window.show();
    return app.exec();
}

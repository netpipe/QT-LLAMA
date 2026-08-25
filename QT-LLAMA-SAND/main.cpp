// Required Qt Modules: QT += core gui widgets network concurrent
// Compile using: qmake && make

#define ENABLE_MACOS_SANDBOX 1

#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QFileDialog>
#include <QLabel>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDesktopServices>
#include <QListWidget>
#include <QTabWidget>
#include <QRegularExpression>
#include <QCheckBox>
#include <QSplitter>
#include <QTimer>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QEventLoop>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QThread>
#include <iostream>

#if defined(__APPLE__) && defined(ENABLE_MACOS_SANDBOX)
#include <sandbox.h>
#include <unistd.h>
#include <sys/wait.h>

// Executes a command inside a strict macOS Seatbelt Sandbox
QString execute_sandboxed_command(const QString &command) {
    int pipefd[2];
    if (pipe(pipefd) == -1) return "Error: Failed to create pipe.";

    pid_t pid = fork();
    if (pid == -1) return "Error: Failed to fork process.";

    if (pid == 0) {
        // --- CHILD PROCESS (Sandboxed) ---
        close(pipefd[0]); 
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        // Seatbelt Profile:
        // - Allow read everywhere
        // - DENY ALL file writes except to /private/tmp
        // - DENY ALL network access
        const char *profile = 
            "(version 1)\n"
            "(allow default)\n"
            "(deny file-write*)\n"
            "(allow file-write* (subpath \"/private/tmp\"))\n"
            "(deny network*)\n"
            "(allow process-exec (subpath \"/bin\"))\n"
            "(allow process-exec (subpath \"/usr/bin\"))\n"
            "(allow mach-lookup)\n"; 

        char *error_buf = NULL;
        if (sandbox_init(profile, SANDBOX_NAMED, &error_buf) != 0) {
            std::cerr << "Sandbox failed: " << error_buf << std::endl;
            sandbox_free_error(error_buf);
            exit(1);
        }

        execl("/bin/sh", "sh", "-c", command.toStdString().c_str(), NULL);
        exit(1); 
    } else {
        // --- PARENT PROCESS (Qt App) ---
        close(pipefd[1]); 
        char buffer[1024];
        QString output = "";
        ssize_t count;
        
        while ((count = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[count] = '\0';
            output.append(QString::fromUtf8(buffer));
        }
        
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        
        return output;
    }
}
#else
// Fallback for Linux/Windows (standard execution)
QString execute_sandboxed_command(const QString &command) {
    QProcess proc;
    proc.start("/bin/sh", QStringList() << "-c" << command);
    proc.waitForFinished();
    return QString::fromUtf8(proc.readAllStandardOutput());
}
#endif

class AgentCoderFrontend : public QWidget {
    Q_OBJECT
public:
    AgentCoderFrontend(QWidget *parent = nullptr) : QWidget(parent) {
        model = "/qwen2.5-3b-instruct-q6_k.gguf";
        modelPath = QApplication::applicationDirPath() + model;
        
        system_prompt = 
            "You are an autonomous AI coding agent (like Hermes or Pi) running on macOS.\n"
            "You have access to a secure, sandboxed bash terminal.\n"
            "1. Think step-by-step. Wrap your reasoning and thoughts in  tags.\n"
            "2. To execute a bash command, wrap ONLY the command in <bash>...</bash> tags.\n"
            "3. You can only execute one command at a time. Wait for the system to return the output.\n"
            "4. If a command fails, analyze the error in  and try a different approach.\n"
            "5. When the task is fully complete, provide a final summary without any tags.\n"
            "Current Task: ";

        setupUI();
        
        llama_proc = new QProcess(this);
        connect(llama_proc, &QProcess::readyReadStandardOutput, this, &AgentCoderFrontend::onLlamaReadyRead);
        connect(llama_proc, &QProcess::readyReadStandardError, this, &AgentCoderFrontend::onLlamaReadyRead);
        connect(llama_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &AgentCoderFrontend::onLlamaFinished);
    }

protected:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (obj == modelLabel && event->type() == QEvent::MouseButtonRelease) {
            chooseModel();
            return true;
        }
        return QWidget::eventFilter(obj, event);
    }

private slots:
    void addTask() {
        QString task = taskInput->text().trimmed();
        if (task.isEmpty()) return;
        task_queue.append(task);
        updateQueueUI();
        taskInput->clear();
    }

    void startAgent() {
        if (task_queue.isEmpty()) {
            QMessageBox::warning(this, "Warning", "Task queue is empty!");
            return;
        }
        is_running = true;
        startAgentBtn->setEnabled(false);
        stopAgentBtn->setEnabled(true);
        statusLabel->setText("Agent Started...");
        callLLM();
    }

    void stopAgent() {
        is_running = false;
        startAgentBtn->setEnabled(true);
        stopAgentBtn->setEnabled(false);
        statusLabel->setText("Status: Idle");
        
        if (llama_proc->state() == QProcess::Running) {
            llama_proc->kill();
            llama_proc->waitForFinished();
        }
    }

    void callLLM() {
        if (!is_running) return;

        if (task_queue.isEmpty() && agent_history.isEmpty()) {
            stopAgent();
            return;
        }

        QString current_task = task_queue.isEmpty() ? "" : task_queue.first();
        
        if (agent_history.isEmpty()) {
            agent_history.append("User Task: " + current_task);
        }

        QString full_prompt = system_prompt + "\n\n" + agent_history.join("\n") + "\nAgent:";
        
        append_action("Calling LLM...");
        statusLabel->setText("Generating...");

        current_llm_output.clear();
        
        QStringList args;
        args << "--flash-attn"
             << "--model" << modelPath
             << "--prompt" << full_prompt
             << "--n-predict" << tokens->text()
             << "-c" << "8192"
             << "--no-display-prompt";

        llama_proc->start(QApplication::applicationDirPath() + "/llama-cli", args);
    }

    void onLlamaReadyRead() {
        QByteArray data = llama_proc->readAllStandardOutput();
        current_llm_output += QString::fromUtf8(data);
        
        QByteArray err = llama_proc->readAllStandardError();
        if(!err.isEmpty()) qDebug() << "LLM STDERR:" << err;
    }

    void onLlamaFinished(int exitCode, QProcess::ExitStatus exitStatus) {
        if (!is_running) return;

        append_action("LLM Generation Complete.");
        
        // Strip prompt if it echoed back
        QString full_prompt = system_prompt + "\n\n" + agent_history.join("\n") + "\nAgent:";
        if (current_llm_output.contains(full_prompt)) {
            current_llm_output.remove(full_prompt);
        }
        if (current_llm_output.startsWith("Agent:")) {
            current_llm_output.remove(0, 6);
        }

        // Parse thinking
        QRegularExpression think_re("(.*?)");
        QRegularExpressionMatchIterator think_it = think_re.globalMatch(current_llm_output);
        while (think_it.hasNext()) {
            QRegularExpressionMatch match = think_it.next();
            append_thinking(match.captured(1).trimmed());
        }

        // Check for bash
        QRegularExpression bash_re("<bash>(.*?)</bash>", QRegularExpression::DotMatchesEverythingOption);
        QRegularExpressionMatch bash_match = bash_re.match(current_llm_output);

        if (bash_match.hasMatch()) {
            QString bash_cmd = bash_match.captured(1).trimmed();
            append_action("Extracted Command: " + bash_cmd);
            agent_history.append("Agent: " + current_llm_output.trimmed());

            if (sandboxCheck->isChecked()) {
                statusLabel->setText("Executing in Sandbox...");
                QFutureWatcher<QString> *watcher = new QFutureWatcher<QString>(this);
                connect(watcher, &QFutureWatcher<QString>::finished, this, [=]() {
                    QString result = watcher->result();
                    append_output("System Output:\n" + result);
                    agent_history.append("System Output:\n" + result);
                    watcher->deleteLater();
                    callLLM(); // Loop back
                });
                QFuture<QString> future = QtConcurrent::run(execute_sandboxed_command, bash_cmd);
                watcher->setFuture(future);
            } else {
                statusLabel->setText("Executing (Unsafe)...");
                QProcess unsafe_proc;
                unsafe_proc.start("/bin/sh", QStringList() << "-c" << bash_cmd);
                unsafe_proc.waitForFinished();
                QString result = QString::fromUtf8(unsafe_proc.readAllStandardOutput());
                append_output("System Output:\n" + result);
                agent_history.append("System Output:\n" + result);
                callLLM();
            }
        } else {
            append_output("Agent Final Response:\n" + current_llm_output);
            agent_history.clear(); 
            
            if (!task_queue.isEmpty()) {
                task_queue.removeFirst();
                updateQueueUI();
            }

            if (!task_queue.isEmpty()) {
                statusLabel->setText("Task Complete. Starting next...");
                QTimer::singleShot(1000, this, &AgentCoderFrontend::callLLM);
            } else {
                stopAgent();
            }
        }
    }

    void chooseModel() {
        QString file = QFileDialog::getOpenFileName(this, "Choose Model", "", "GGUF Model (*.gguf)");
        if (!file.isEmpty()) {
            modelPath = file;
            modelLabel->setText("Model: " + modelPath);
        }
    }

    void downloadModel() {
        if (!QFileInfo::exists(modelPath)){
            QString initialUrl = "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q6_k.gguf";
            QString outputFile = modelPath;
            
            QNetworkRequest request(initialUrl);
            QNetworkReply *reply = manager->get(request);
            
            QObject::connect(reply, &QNetworkReply::finished, [=] {
                QVariant redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
                if (redirect.isValid()) {
                    QUrl redirectedUrl = redirect.toUrl();
                    QNetworkRequest newReq(redirectedUrl);
                    QNetworkReply *finalReply = manager->get(newReq);
                    QObject::connect(finalReply, &QNetworkReply::finished, [=] {
                        if (finalReply->error() == QNetworkReply::NoError) {
                            QFile file(outputFile);
                            if (file.open(QIODevice::WriteOnly)) {
                                file.write(finalReply->readAll());
                                file.close();
                                taskInput->setText("Model downloaded successfully!");
                            }
                        }
                        finalReply->deleteLater();
                    });
                } else {
                    if (reply->error() == QNetworkReply::NoError) {
                        QFile file(outputFile);
                        if (file.open(QIODevice::WriteOnly)) {
                            file.write(reply->readAll());
                            file.close();
                            taskInput->setText("Model downloaded directly!");
                        }
                    }
                }
                reply->deleteLater();
            });
            taskInput->setText("Downloading Qwen 2.5 3B...");
        } else { 
            taskInput->setText("Model already exists.");
        }
    }

private:
    void setupUI() {
        auto *mainLayout = new QVBoxLayout(this);

        auto *topLayout = new QHBoxLayout();
        modelLabel = new QLabel("Model: " + modelPath, this);
        modelLabel->setStyleSheet("color: blue; text-decoration: underline;");
        modelLabel->setCursor(Qt::PointingHandCursor);
        modelLabel->installEventFilter(this);
        topLayout->addWidget(modelLabel);

        tokens = new QLineEdit(this);
        tokens->setText("1024"); 
        tokens->setFixedWidth(60);
        topLayout->addWidget(new QLabel("Max Tokens:"));
        topLayout->addWidget(tokens);

        sandboxCheck = new QCheckBox("Enable Sandbox", this);
        sandboxCheck->setChecked(true);
        topLayout->addWidget(sandboxCheck);

        auto *downloadBtn = new QPushButton("Download Qwen 2.5 3B", this);
        connect(downloadBtn, &QPushButton::clicked, this, &AgentCoderFrontend::downloadModel);
        topLayout->addWidget(downloadBtn);
        topLayout->addStretch();
        mainLayout->addLayout(topLayout);

        QSplitter *splitter = new QSplitter(Qt::Horizontal, this);

        QWidget *queueWidget = new QWidget();
        auto *queueLayout = new QVBoxLayout(queueWidget);
        queueLayout->addWidget(new QLabel("Task Queue:"));
        taskQueueList = new QListWidget();
        queueLayout->addWidget(taskQueueList);

        auto *addTaskLayout = new QHBoxLayout();
        taskInput = new QLineEdit();
        taskInput->setPlaceholderText("e.g., Write a python script to scrape...");
        addTaskLayout->addWidget(taskInput);
        addTaskBtn = new QPushButton("Add Task");
        connect(addTaskBtn, &QPushButton::clicked, this, &AgentCoderFrontend::addTask);
        connect(taskInput, &QLineEdit::returnPressed, this, &AgentCoderFrontend::addTask);
        addTaskLayout->addWidget(addTaskBtn);
        queueLayout->addLayout(addTaskLayout);

        splitter->addWidget(queueWidget);

        outputTabs = new QTabWidget();
        thinkingView = new QPlainTextEdit();
        thinkingView->setReadOnly(true);
        thinkingView->setStyleSheet("QPlainTextEdit { background-color: #2d2d2d; color: #a0a0a0; font-style: italic; }");
        outputTabs->addTab(thinkingView, "Thinking Process");

        actionsView = new QPlainTextEdit();
        actionsView->setReadOnly(true);
        actionsView->setStyleSheet("QPlainTextEdit { background-color: #1e1e1e; color: #d4d4d4; font-family: monospace; }");
        outputTabs->addTab(actionsView, "Actions & Bash");

        outputView = new QPlainTextEdit();
        outputView->setReadOnly(true);
        outputView->setStyleSheet("QPlainTextEdit { background-color: #ffffff; color: #000000; }");
        outputTabs->addTab(outputView, "Console Output");

        splitter->addWidget(outputTabs);
        splitter->setSizes({200, 600});
        mainLayout->addWidget(splitter);

        auto *bottomLayout = new QHBoxLayout();
        startAgentBtn = new QPushButton("Start Agent");
        stopAgentBtn = new QPushButton("Stop Agent");
        stopAgentBtn->setEnabled(false);
        statusLabel = new QLabel("Status: Idle");
        statusLabel->setStyleSheet("font-weight: bold; color: #555;");

        connect(startAgentBtn, &QPushButton::clicked, this, &AgentCoderFrontend::startAgent);
        connect(stopAgentBtn, &QPushButton::clicked, this, &AgentCoderFrontend::stopAgent);

        bottomLayout->addWidget(startAgentBtn);
        bottomLayout->addWidget(stopAgentBtn);
        bottomLayout->addStretch();
        bottomLayout->addWidget(statusLabel);
        mainLayout->addLayout(bottomLayout);
    }

    void updateQueueUI() {
        taskQueueList->clear();
        for(const QString &task : task_queue) {
            taskQueueList->addItem(task);
        }
    }

    void append_thinking(const QString &text) {
        thinkingView->appendPlainText(text + "\n");
        thinkingView->moveCursor(QTextCursor::End);
        outputTabs->setCurrentWidget(thinkingView);
    }

    void append_action(const QString &text) {
        actionsView->appendPlainText(text + "\n");
        actionsView->moveCursor(QTextCursor::End);
        outputTabs->setCurrentWidget(actionsView);
    }

    void append_output(const QString &text) {
        outputView->appendPlainText(text + "\n");
        outputView->moveCursor(QTextCursor::End);
        outputTabs->setCurrentWidget(outputView);
    }

    QLabel *modelLabel;
    QLineEdit *tokens;
    QListWidget *taskQueueList;
    QLineEdit *taskInput;
    QPushButton *addTaskBtn;
    QPushButton *startAgentBtn;
    QPushButton *stopAgentBtn;
    QTabWidget *outputTabs;
    QPlainTextEdit *thinkingView;
    QPlainTextEdit *actionsView;
    QPlainTextEdit *outputView;
    QLabel *statusLabel;
    QCheckBox *sandboxCheck;

    QProcess *llama_proc;
    QNetworkAccessManager *manager = new QNetworkAccessManager(this);
    QStringList agent_history;
    QStringList task_queue;
    bool is_running = false;
    QString modelPath;
    QString model;
    QString system_prompt;
    QString current_llm_output;
};

#include "main.moc"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    AgentCoderFrontend window;
    window.setWindowTitle("Qwen 2.5 Agent Coder (Hermes/Pi Style)");
    window.resize(1000, 700);
    window.show();
    return app.exec();
}

#pragma once

#include <QList>
#include <QMutex>
#include <QStringList>
#include <QThread>
#include <QWaitCondition>

#include <atomic>
#include <memory>

struct Job {
    QString id, label;
    QString source = "pixiv";  // pixiv / wayback
    QStringList ids;           // pixiv: 小説 ID / Wayback: 元の URL
    double wait = 3;
    std::atomic<bool> cancel{false};
    // 以下は画面側 (GUI スレッド) だけが触る
    QString status = "queued";  // queued / running / done / cancelled
    int done = 0, failed = 0;
};

// 保存ジョブを 1 本のスレッドで順番に処理する (pixiv への同時アクセスを避けるため)
class JobWorker : public QThread {
    Q_OBJECT
public:
    using QThread::QThread;
    void enqueue(std::shared_ptr<Job> job);
    void shutdown();

signals:
    void jobStarted(const QString& id);
    void progress(const QString& id, int done, int failed, const QString& savedId);
    void log(const QString& id, const QString& message);
    void jobFinished(const QString& id, bool cancelled);

protected:
    void run() override;

private:
    QMutex mutex_;
    QWaitCondition cond_;
    QList<std::shared_ptr<Job>> queue_;
    std::shared_ptr<Job> current_;
    bool stop_ = false;
};

#include "jobs.h"

#include "pixiv.h"
#include "wayback.h"

#include <QHash>
#include <QSet>

void JobWorker::enqueue(std::shared_ptr<Job> job)
{
    QMutexLocker lock(&mutex_);
    queue_ << std::move(job);
    cond_.wakeOne();
}

void JobWorker::shutdown()
{
    {
        QMutexLocker lock(&mutex_);
        stop_ = true;
        for (auto& j : queue_)
            j->cancel = true;
        if (current_)
            current_->cancel = true;
        cond_.wakeOne();
    }
    if (!wait(10000))
        terminate();
}

void JobWorker::run()
{
    for (;;) {
        std::shared_ptr<Job> job;
        {
            QMutexLocker lock(&mutex_);
            while (queue_.isEmpty() && !stop_)
                cond_.wait(&mutex_);
            if (stop_)
                return;
            job = current_ = queue_.takeFirst();
        }
        if (job->cancel) {
            emit jobFinished(job->id, true);
            continue;
        }
        emit jobStarted(job->id);

        const bool isWayback = job->source == "wayback";
        pixiv::Http http(isWayback ? pixiv::Session{} : pixiv::Session::load(), &job->cancel);
        QHash<QString, QString> existing;
        if (!isWayback)
            for (const auto& n : pixiv::library())
                existing.insert(n.id, n.dir);
        QSet<QString> siteDirs;
        int done = 0, failed = 0;
        const auto logFn = [&](const QString& m) { emit log(job->id, m); };
        http.setLog(logFn);

        for (qsizetype i = 0; i < job->ids.size() && !job->cancel; ++i) {
            const QString id = job->ids[i];
            try {
                if (i)
                    http.wait(job->wait);
                QString title;
                if (isWayback) {
                    const auto saved = wayback::savePage(http, id, logFn);
                    siteDirs.insert(saved.dir);
                    title = saved.title;
                } else {
                    title = pixiv::saveNovel(http, id, existing.value(id), logFn).title;
                }
                emit log(job->id, "保存: " + title);
                emit progress(job->id, ++done, failed, id);
            } catch (const net::Error& e) {
                if (job->cancel)
                    break;
                if (e.msg == wayback::kArchiveOffline) {
                    // 残りのページも同じ理由で失敗するので、ここで打ち切る (保存済みの分は残る)
                    emit log(job->id, e.msg);
                    job->cancel = true;
                    break;
                }
                emit log(job->id, "失敗: " + id + ": " + e.msg);
                emit progress(job->id, done, ++failed, QString());
            } catch (const std::exception& e) {
                emit log(job->id, "失敗: " + id + ": " + QString::fromUtf8(e.what()));
                emit progress(job->id, done, ++failed, QString());
            }
        }
        // 閲覧用ページのリンクは保存したページ全体を見て張り替えるので、最後にまとめて作る
        for (const auto& dir : std::as_const(siteDirs)) {
            try {
                wayback::renderSite(dir);
            } catch (const net::Error& e) {
                emit log(job->id, "閲覧用ページの作成に失敗: " + e.msg);
            }
        }
        {
            QMutexLocker lock(&mutex_);
            current_.reset();
        }
        emit jobFinished(job->id, job->cancel);
    }
}

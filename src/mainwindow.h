#pragma once

#include "jobs.h"
#include "pixiv.h"
#include "wayback.h"

#include <QHash>
#include <QIcon>
#include <QMainWindow>

#include <memory>

class ChromeLogin;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;
    void loadUrl(const QString& url);  // URL を入れて作品一覧を読み込む

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    QWidget* buildImportTab();
    QWidget* buildLibraryTab();
    void buildMenu();

    // 取り込み
    void resolve();
    void showResult();
    void selectBy(const QString& mode);
    QStringList checkedIds() const;
    void updateSaveButton();
    void startJob();
    void markSaved(const QString& id);

    // ジョブ
    QTreeWidgetItem* jobItem(const QString& id) const;
    void refreshJob(const QString& id);
    void cancelSelectedJobs();
    void clearFinishedJobs();
    bool hasActiveJobs() const;

    // ライブラリ
    void reloadLibrary();
    void renderLibrary();
    void libraryMenu(const QPoint& pos);
    void openNovel(const QString& dir);
    void openFolder(const QString& dir);
    void checkUpdates(const QString& userId);
    QIcon coverIcon(const QString& path);

    // ログイン
    void updateLogin();
    void changeLibraryRoot();

    QTabWidget* tabs_ = nullptr;
    QLabel* loginLabel_ = nullptr;
    QPushButton* loginBtn_ = nullptr;

    QLineEdit* urlEdit_ = nullptr;
    QPushButton* resolveBtn_ = nullptr;
    QLabel* resultTitle_ = nullptr;
    QLabel* resultSummary_ = nullptr;
    QLabel* warningLabel_ = nullptr;
    QPushButton* scopeBtn_ = nullptr;
    QWidget* selectionBar_ = nullptr;
    QTreeWidget* itemsTree_ = nullptr;
    QSpinBox* waitSpin_ = nullptr;
    QPushButton* saveBtn_ = nullptr;
    QTreeWidget* jobsTree_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;

    QLineEdit* searchEdit_ = nullptr;
    QLabel* libCount_ = nullptr;
    QLabel* rootLabel_ = nullptr;
    QTreeWidget* libTree_ = nullptr;

    ChromeLogin* login_ = nullptr;
    JobWorker worker_;
    QHash<QString, std::shared_ptr<Job>> jobs_;
    ResolveResult current_;
    QList<pixiv::LibNovel> lib_;
    QList<wayback::LibSite> sites_;
    QHash<QString, QIcon> coverCache_;
    bool resolving_ = false;
};

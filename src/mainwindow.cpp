#include "mainwindow.h"

#include "chromelogin.h"
#include "paths.h"
#include "wayback.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTime>
#include <QTreeWidget>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

namespace {

enum Role { IdRole = Qt::UserRole, KindRole, UserRole, DirRole };
enum ItemCol { ColTitle, ColR18, ColStatus, ColChars, ColDate };
enum LibCol { LibTitle, LibChars, LibDate, LibTags };

const QColor kSaved(46, 157, 79), kUpdated(196, 138, 0), kR18(209, 36, 90), kMissing(140, 140, 140);

QString kb(qint64 bytes) { return bytes > 0 ? QString("%1 KB").arg(qMax<qint64>(1, (bytes + 512) / 1024)) : QString(); }

QString ymd(const QString& iso)
{
    const QDateTime d = QDateTime::fromString(iso, Qt::ISODate);
    return d.isValid() ? d.toLocalTime().toString("yyyy/MM/dd") : QString();
}

QString num(qint64 n) { return QLocale(QLocale::Japanese).toString(n); }

QLabel* hint(const QString& text)
{
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet("color: palette(placeholder-text);");
    return l;
}

}  // namespace

MainWindow::MainWindow()
{
    setWindowTitle("ScrapeWay");
    resize(1120, 780);

    auto* central = new QWidget;
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(12, 8, 12, 8);

    auto* head = new QHBoxLayout;
    auto* brand = new QLabel("ScrapeWay");
    QFont bf = brand->font();
    bf.setPointSizeF(bf.pointSizeF() * 1.4);
    bf.setBold(true);
    brand->setFont(bf);
    head->addWidget(brand);
    head->addSpacing(12);
    auto* sub = hint("pixiv・Wayback Machine の小説の取り込み・保存");
    sub->setWordWrap(false);
    head->addWidget(sub);
    head->addStretch();
    loginLabel_ = new QLabel;
    loginBtn_ = new QPushButton("ログイン");
    head->addWidget(loginLabel_);
    head->addWidget(loginBtn_);
    v->addLayout(head);

    tabs_ = new QTabWidget;
    tabs_->addTab(buildImportTab(), "取り込み");
    tabs_->addTab(buildLibraryTab(), "ライブラリ");
    v->addWidget(tabs_, 1);
    setCentralWidget(central);
    buildMenu();
    statusBar();  // 最初から表示しておく (後から作るとレイアウトが遅れて表示されないことがある)

    login_ = new ChromeLogin(this);
    connect(loginBtn_, &QPushButton::clicked, this, [this] {
        login_->start();
        updateLogin();
    });
    connect(login_, &ChromeLogin::status, this, [this](const QString& m) {
        statusBar()->showMessage(m);
        updateLogin();
    });
    connect(login_, &ChromeLogin::finished, this, [this](bool ok, const QString& m) {
        statusBar()->showMessage(m, 10000);
        updateLogin();
        if (!ok)
            QMessageBox::information(this, "ログイン", m);
    });

    connect(&worker_, &JobWorker::jobStarted, this, [this](const QString& id) {
        if (auto j = jobs_.value(id)) {
            j->status = "running";
            refreshJob(id);
        }
    });
    connect(&worker_, &JobWorker::progress, this, [this](const QString& id, int done, int failed, const QString& saved) {
        if (auto j = jobs_.value(id)) {
            j->done = done;
            j->failed = failed;
            refreshJob(id);
        }
        if (!saved.isEmpty())
            markSaved(saved);
    });
    connect(&worker_, &JobWorker::log, this, [this](const QString&, const QString& m) {
        logView_->appendPlainText(QTime::currentTime().toString("HH:mm:ss ") + m);
    });
    connect(&worker_, &JobWorker::jobFinished, this, [this](const QString& id, bool cancelled) {
        if (auto j = jobs_.value(id)) {
            j->status = cancelled ? "cancelled" : "done";
            refreshJob(id);
        }
        reloadLibrary();
    });
    worker_.start();

    updateLogin();
    // 閲覧用 HTML を最新の形式に作り直してからライブラリを読む
    auto* w = new QFutureWatcher<void>(this);
    connect(w, &QFutureWatcher<void>::finished, this, [this, w] {
        w->deleteLater();
        reloadLibrary();
    });
    w->setFuture(QtConcurrent::run([] {
        pixiv::rebuildAll();
        wayback::rebuildAll();
    }));
}

MainWindow::~MainWindow() { worker_.shutdown(); }

// ---------- 画面の組み立て ----------
QWidget* MainWindow::buildImportTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);

    auto* row = new QHBoxLayout;
    urlEdit_ = new QLineEdit;
    urlEdit_->setPlaceholderText("pixiv の作品・作者の URL、または Wayback Machine（web.archive.org）のページの URL を貼り付け");
    urlEdit_->setClearButtonEnabled(true);
    auto* paste = new QPushButton("貼り付け");
    resolveBtn_ = new QPushButton("読み込む");
    resolveBtn_->setDefault(true);
    row->addWidget(urlEdit_, 1);
    row->addWidget(paste);
    row->addWidget(resolveBtn_);
    v->addLayout(row);
    v->addWidget(hint("pixiv: https://www.pixiv.net/users/123（全作品） ／ …/novel/series/456（シリーズ） ／ …/novel/show.php?id=789（1話）\n"
                      "Wayback: https://web.archive.org/web/20050101000000/http://example.com/novel/（目次などのページ。リンク先と同じフォルダ内のページを一覧にします）"));
    connect(paste, &QPushButton::clicked, this, [this] {
        urlEdit_->setText(QApplication::clipboard()->text().trimmed());
        resolve();
    });
    connect(resolveBtn_, &QPushButton::clicked, this, &MainWindow::resolve);
    connect(urlEdit_, &QLineEdit::returnPressed, this, &MainWindow::resolve);

    auto* split = new QSplitter(Qt::Vertical);

    auto* resultBox = new QWidget;
    auto* rv = new QVBoxLayout(resultBox);
    rv->setContentsMargins(0, 8, 0, 0);
    resultTitle_ = new QLabel;
    QFont tf = resultTitle_->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() * 1.15);
    resultTitle_->setFont(tf);
    resultTitle_->setWordWrap(true);
    resultTitle_->setText("URL を入力して「読み込む」を押すと、作品の一覧が表示されます");
    resultSummary_ = hint("Chrome で URL をコピーしてからこのウィンドウに戻ると、自動で入力されます");
    warningLabel_ = new QLabel;
    warningLabel_->setWordWrap(true);
    warningLabel_->setStyleSheet(QString("color: %1;").arg(kUpdated.name()));
    warningLabel_->hide();
    scopeBtn_ = new QPushButton;
    scopeBtn_->hide();
    connect(scopeBtn_, &QPushButton::clicked, this, [this] { loadUrl(current_.parentUrl); });
    auto* scopeRow = new QHBoxLayout;
    scopeRow->addWidget(resultSummary_, 1);
    scopeRow->addWidget(scopeBtn_);
    rv->addWidget(resultTitle_);
    rv->addLayout(scopeRow);
    rv->addWidget(warningLabel_);

    selectionBar_ = new QWidget;
    auto* sb = new QHBoxLayout(selectionBar_);
    sb->setContentsMargins(0, 0, 0, 0);
    const QList<std::pair<QString, QString>> modes = {{"todo", "未保存と更新ありを選択"}, {"all", "すべて選択"}, {"none", "選択解除"}};
    for (const auto& [mode, label] : modes) {
        auto* b = new QPushButton(label);
        connect(b, &QPushButton::clicked, this, [this, m = mode] { selectBy(m); });
        sb->addWidget(b);
    }
    sb->addStretch();
    sb->addWidget(new QLabel("取得間隔"));
    waitSpin_ = new QSpinBox;
    waitSpin_->setRange(1, 60);
    waitSpin_->setSuffix(" 秒");
    waitSpin_->setValue(paths::defaultWait());
    sb->addWidget(waitSpin_);
    saveBtn_ = new QPushButton("保存");
    QFont sf = saveBtn_->font();
    sf.setBold(true);
    saveBtn_->setFont(sf);
    sb->addWidget(saveBtn_);
    connect(saveBtn_, &QPushButton::clicked, this, &MainWindow::startJob);
    selectionBar_->hide();
    rv->addWidget(selectionBar_);

    itemsTree_ = new QTreeWidget;
    itemsTree_->setHeaderLabels({"タイトル", "", "状態", "文字数", "更新日"});
    itemsTree_->setUniformRowHeights(true);
    itemsTree_->header()->setStretchLastSection(false);
    itemsTree_->header()->setSectionResizeMode(ColTitle, QHeaderView::Stretch);
    for (int c : {ColR18, ColStatus, ColChars, ColDate})
        itemsTree_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    connect(itemsTree_, &QTreeWidget::itemChanged, this, &MainWindow::updateSaveButton);
    rv->addWidget(itemsTree_, 1);
    split->addWidget(resultBox);

    auto* jobsBox = new QWidget;
    auto* jv = new QVBoxLayout(jobsBox);
    jv->setContentsMargins(0, 8, 0, 0);
    auto* jh = new QHBoxLayout;
    auto* jl = new QLabel("保存の進行状況");
    QFont jf = jl->font();
    jf.setBold(true);
    jl->setFont(jf);
    jh->addWidget(jl);
    jh->addStretch();
    auto* cancel = new QPushButton("選択した作業を中止");
    auto* clear = new QPushButton("完了したものを消す");
    jh->addWidget(cancel);
    jh->addWidget(clear);
    connect(cancel, &QPushButton::clicked, this, &MainWindow::cancelSelectedJobs);
    connect(clear, &QPushButton::clicked, this, &MainWindow::clearFinishedJobs);
    jv->addLayout(jh);

    auto* jsplit = new QSplitter(Qt::Horizontal);
    jobsTree_ = new QTreeWidget;
    jobsTree_->setHeaderLabels({"内容", "状態", "進捗"});
    jobsTree_->setRootIsDecorated(false);
    jobsTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    jobsTree_->header()->setStretchLastSection(false);
    jobsTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    jobsTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    jobsTree_->setColumnWidth(2, 160);
    logView_ = new QPlainTextEdit;
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(3000);
    logView_->setPlaceholderText("ログ");
    jsplit->addWidget(jobsTree_);
    jsplit->addWidget(logView_);
    jsplit->setSizes({520, 480});
    jv->addWidget(jsplit, 1);
    split->addWidget(jobsBox);
    split->setSizes({480, 220});
    v->addWidget(split, 1);
    return page;
}

QWidget* MainWindow::buildLibraryTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    auto* row = new QHBoxLayout;
    searchEdit_ = new QLineEdit;
    searchEdit_->setPlaceholderText("タイトル・作者・シリーズ・タグで検索");
    searchEdit_->setClearButtonEnabled(true);
    libCount_ = new QLabel;
    auto* reload = new QPushButton("再読み込み");
    auto* openRoot = new QPushButton("保存先を開く");
    row->addWidget(searchEdit_, 1);
    row->addWidget(libCount_);
    row->addWidget(reload);
    row->addWidget(openRoot);
    v->addLayout(row);
    connect(searchEdit_, &QLineEdit::textChanged, this, &MainWindow::renderLibrary);
    connect(reload, &QPushButton::clicked, this, &MainWindow::reloadLibrary);
    connect(openRoot, &QPushButton::clicked, this, [this] {
        QDir().mkpath(pixiv::pixivRoot());
        openFolder(pixiv::pixivRoot());
    });

    libTree_ = new QTreeWidget;
    libTree_->setHeaderLabels({"タイトル", "文字数", "更新日", "タグ"});
    libTree_->setIconSize(QSize(30, 40));
    libTree_->header()->setStretchLastSection(true);
    libTree_->setColumnWidth(LibTitle, 460);
    libTree_->setColumnWidth(LibChars, 90);
    libTree_->setColumnWidth(LibDate, 100);
    libTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(libTree_, &QTreeWidget::customContextMenuRequested, this, &MainWindow::libraryMenu);
    connect(libTree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* it) {
        const QString kind = it->data(0, KindRole).toString();
        if (kind == "novel")
            openNovel(it->data(0, DirRole).toString());
        else if (kind == "page")
            QDesktopServices::openUrl(QUrl::fromLocalFile(it->data(0, DirRole).toString()));
    });
    v->addWidget(libTree_, 1);
    rootLabel_ = hint("");
    v->addWidget(hint("ダブルクリックでブラウザで読む（縦書き・文字サイズ・配色を切り替えられます）／右クリックでメニュー"));
    v->addWidget(rootLabel_);
    return page;
}

void MainWindow::buildMenu()
{
    auto* file = menuBar()->addMenu("ファイル(&F)");
    file->addAction("保存先フォルダを変更…", this, &MainWindow::changeLibraryRoot);
    file->addAction("保存先フォルダを開く", this, [this] {
        QDir().mkpath(pixiv::pixivRoot());
        openFolder(pixiv::pixivRoot());
    });
    file->addSeparator();
    file->addAction("終了", this, &QWidget::close);

    auto* account = menuBar()->addMenu("アカウント(&A)");
    account->addAction("pixiv にログイン…", this, [this] {
        login_->start();
        updateLogin();
    });
    account->addAction("ログイン情報を削除", this, [this] {
        if (QMessageBox::question(this, "ログイン情報を削除", "保存した pixiv のログイン情報を削除しますか？") == QMessageBox::Yes) {
            pixiv::Session::clear();
            updateLogin();
        }
    });

    auto* help = menuBar()->addMenu("ヘルプ(&H)");
    help->addAction("使い方（取扱説明書）", this, [this] {
        // 配布版は exe の隣、開発中はソースの docs/ にある
        const QDir exe(QApplication::applicationDirPath());
        for (const QString& p : {exe.filePath("manual.html"), exe.filePath("../docs/manual.html"), exe.filePath("../../docs/manual.html")}) {
            if (QFileInfo::exists(p)) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(p).absoluteFilePath()));
                return;
            }
        }
        QMessageBox::information(this, "使い方", "取扱説明書（manual.html）が見つかりません。ScrapeWay.exe と同じフォルダにあるか確認してください。");
    });
    help->addAction("ScrapeWay について", this, [this] {
        QMessageBox::about(this, "ScrapeWay",
                           QString("<b>ScrapeWay %1</b><p>pixiv の小説を投稿時の形式のまま保存して、オフラインで読むためのアプリです。</p>"
                                   "<p>保存先: %2<br>設定: %3</p>")
                               .arg(QApplication::applicationVersion(),
                                    QDir::toNativeSeparators(paths::libraryRoot()).toHtmlEscaped(),
                                    QDir::toNativeSeparators(paths::dataDir()).toHtmlEscaped()));
    });
}

// ---------- ログイン ----------
void MainWindow::updateLogin()
{
    const pixiv::Session s = pixiv::Session::load();
    if (login_ && login_->running()) {
        loginLabel_->setText(QString("<span style='color:%1'>●</span> ログイン待ち…").arg(kUpdated.name()));
        loginBtn_->setEnabled(false);
        return;
    }
    loginBtn_->setEnabled(true);
    if (s.valid()) {
        loginLabel_->setText(QString("<span style='color:%1'>●</span> pixiv: %2")
                                 .arg(kSaved.name(), (s.userName.isEmpty() ? "ID " + s.userId : s.userName).toHtmlEscaped()));
        loginBtn_->setText("再ログイン");
    } else {
        loginLabel_->setText("<span style='color:gray'>●</span> pixiv: 未ログイン");
        loginBtn_->setText("ログイン");
    }
}

void MainWindow::changeLibraryRoot()
{
    if (hasActiveJobs()) {
        QMessageBox::information(this, "保存先の変更", "保存中の作業が終わってから変更してください");
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(this, "保存先フォルダを選択", paths::libraryRoot());
    if (dir.isEmpty())
        return;
    paths::setLibraryRoot(dir);
    coverCache_.clear();
    reloadLibrary();
}

// ---------- 取り込み ----------
void MainWindow::loadUrl(const QString& url)
{
    urlEdit_->setText(url.trimmed());
    tabs_->setCurrentIndex(0);
    resolve();
}

void MainWindow::resolve()
{
    const QString url = urlEdit_->text().trimmed();
    if (url.isEmpty() || resolving_)
        return;
    const bool isWayback = !pixiv::isPixivUrl(url);
    resolving_ = true;
    resolveBtn_->setEnabled(false);
    resultTitle_->setText(isWayback ? "Wayback Machine で保存されたページを調べています…（数十秒かかることがあります）"
                                    : "作品一覧を読み込んでいます…");
    resultSummary_->clear();
    warningLabel_->hide();
    scopeBtn_->hide();
    selectionBar_->hide();
    itemsTree_->clear();

    auto* w = new QFutureWatcher<ResolveResult>(this);
    connect(w, &QFutureWatcher<ResolveResult>::finished, this, [this, w] {
        const ResolveResult r = w->result();
        w->deleteLater();
        resolving_ = false;
        resolveBtn_->setEnabled(true);
        if (!r.error.isEmpty()) {
            current_ = {};
            resultTitle_->setText("読み込めませんでした");
            warningLabel_->setText(r.error);
            warningLabel_->show();
            return;
        }
        current_ = r;
        showResult();
    });
    w->setFuture(QtConcurrent::run([url, isWayback] {
        ResolveResult r;
        try {
            if (isWayback) {
                net::Http http;
                r = wayback::resolve(http, url);
            } else {
                pixiv::Http http;
                r = pixiv::resolve(http, url);
            }
        } catch (const net::Error& e) {
            r.error = e.msg;
        } catch (const std::exception& e) {
            r.error = QString::fromUtf8(e.what());
        }
        return r;
    }));
}

void MainWindow::showResult()
{
    const auto& items = current_.items;
    const bool wb = current_.kind == "wayback";
    const QString unit = wb ? "ページ" : "話";
    auto count = [&](const QString& s) { return std::count_if(items.begin(), items.end(), [&](const auto& i) { return i.status == s; }); };
    resultTitle_->setText(current_.title);
    QString summary = QString("%1%2 ・ 未保存 %3 ・ ").arg(items.size()).arg(unit).arg(count("new"));
    summary += wb ? QString("Wayback になし %1 ・ ").arg(count("missing")) : QString("更新あり %1 ・ ").arg(count("updated"));
    summary += QString("保存済み %1").arg(count("saved"));
    resultSummary_->setText(summary);
    warningLabel_->setText(current_.warning);
    warningLabel_->setVisible(!current_.warning.isEmpty());
    selectionBar_->setVisible(!items.isEmpty());
    scopeBtn_->setVisible(!current_.parentUrl.isEmpty());
    scopeBtn_->setText("一つ上のフォルダも含めて一覧にする");
    itemsTree_->setHeaderLabels({wb ? "ページ" : "タイトル", "", "状態", wb ? "サイズ" : "文字数", wb ? "最初の記録" : "更新日"});
    itemsTree_->setColumnHidden(ColR18, wb);

    QSignalBlocker block(itemsTree_);
    itemsTree_->clear();
    QHash<QString, QTreeWidgetItem*> groups;
    for (const auto& it : items) {
        QTreeWidgetItem*& g = groups[it.seriesId];
        if (!g) {
            g = new QTreeWidgetItem(itemsTree_);
            g->setText(ColTitle, wb ? it.seriesTitle : it.seriesId.isEmpty() ? "単発作品" : "シリーズ「" + it.seriesTitle + "」");
            g->setFlags(g->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            g->setCheckState(ColTitle, Qt::Unchecked);
            QFont f = g->font(ColTitle);
            f.setBold(true);
            g->setFont(ColTitle, f);
        }
        auto* row = new QTreeWidgetItem(g);
        row->setText(ColTitle, (it.order ? QString("#%1  ").arg(it.order) : QString()) + it.title);
        row->setToolTip(ColTitle, (wb ? it.id : it.title) + (it.tags.isEmpty() || wb ? QString() : "\n" + it.tags.join(" ")));
        row->setData(ColTitle, IdRole, it.id);
        row->setData(ColTitle, KindRole, it.status);
        row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
        if (it.r18) {
            row->setText(ColR18, "R-18");
            row->setForeground(ColR18, kR18);
        }
        static const QHash<QString, std::pair<QString, QColor>> badges = {
            {"saved", {"保存済み", kSaved}}, {"updated", {"更新あり", kUpdated}}, {"missing", {"Wayback になし", kMissing}}};
        if (badges.contains(it.status)) {
            row->setText(ColStatus, badges[it.status].first);
            row->setForeground(ColStatus, badges[it.status].second);
        }
        if (it.status == "missing")
            row->setForeground(ColTitle, kMissing);
        row->setText(ColChars, wb ? kb(it.textCount) : num(it.textCount) + "字");
        row->setTextAlignment(ColChars, Qt::AlignRight | Qt::AlignVCenter);
        row->setText(ColDate, ymd(it.updateDate.isEmpty() ? it.createDate : it.updateDate));
        const bool todo = it.status == "new" || it.status == "updated";
        row->setCheckState(ColTitle, todo && it.preselect ? Qt::Checked : Qt::Unchecked);
    }
    for (auto* g : std::as_const(groups))
        g->setText(ColTitle, g->text(ColTitle) + QString("（%1%2）").arg(g->childCount()).arg(unit));
    itemsTree_->expandAll();
    block.unblock();
    updateSaveButton();
}

void MainWindow::selectBy(const QString& mode)
{
    QSignalBlocker block(itemsTree_);
    for (int i = 0; i < itemsTree_->topLevelItemCount(); ++i) {
        auto* g = itemsTree_->topLevelItem(i);
        for (int k = 0; k < g->childCount(); ++k) {
            auto* row = g->child(k);
            const QString st = row->data(ColTitle, KindRole).toString();
            const bool on = mode == "all" || (mode == "todo" && (st == "new" || st == "updated"));
            row->setCheckState(ColTitle, on ? Qt::Checked : Qt::Unchecked);
        }
    }
    block.unblock();
    itemsTree_->viewport()->update();
    updateSaveButton();
}

QStringList MainWindow::checkedIds() const
{
    QStringList ids;
    for (int i = 0; i < itemsTree_->topLevelItemCount(); ++i) {
        auto* g = itemsTree_->topLevelItem(i);
        for (int k = 0; k < g->childCount(); ++k)
            if (g->child(k)->checkState(ColTitle) == Qt::Checked)
                ids << g->child(k)->data(ColTitle, IdRole).toString();
    }
    return ids;
}

void MainWindow::updateSaveButton()
{
    const qsizetype n = checkedIds().size();
    saveBtn_->setText(QString("選択した %1 %2を保存").arg(n).arg(current_.kind == "wayback" ? "ページ" : "話"));
    saveBtn_->setEnabled(n > 0);
}

void MainWindow::startJob()
{
    const QStringList ids = checkedIds();
    if (ids.isEmpty())
        return;
    const bool wb = current_.kind == "wayback";
    auto job = std::make_shared<Job>();
    job->id = QUuid::createUuid().toString(QUuid::Id128).left(8);
    job->source = wb ? "wayback" : "pixiv";
    job->label = QString("%1（%2%3）").arg(current_.title).arg(ids.size()).arg(wb ? "ページ" : "話");
    job->ids = ids;
    job->wait = waitSpin_->value();
    paths::setDefaultWait(waitSpin_->value());
    jobs_.insert(job->id, job);

    auto* row = new QTreeWidgetItem;
    row->setText(0, job->label);
    row->setToolTip(0, job->label);
    row->setData(0, IdRole, job->id);
    jobsTree_->insertTopLevelItem(0, row);
    auto* bar = new QProgressBar;
    bar->setRange(0, int(ids.size()));
    bar->setValue(0);
    bar->setFormat("%v / %m");
    jobsTree_->setItemWidget(row, 2, bar);
    refreshJob(job->id);
    worker_.enqueue(job);
    statusBar()->showMessage(QString("%1件の保存を開始しました").arg(ids.size()), 5000);
}

void MainWindow::markSaved(const QString& id)
{
    for (auto& it : current_.items)
        if (it.id == id)
            it.status = "saved";
    QSignalBlocker block(itemsTree_);
    for (int i = 0; i < itemsTree_->topLevelItemCount(); ++i) {
        auto* g = itemsTree_->topLevelItem(i);
        for (int k = 0; k < g->childCount(); ++k) {
            auto* row = g->child(k);
            if (row->data(ColTitle, IdRole).toString() == id) {
                row->setText(ColStatus, "保存済み");
                row->setForeground(ColStatus, kSaved);
                row->setData(ColTitle, KindRole, "saved");
                row->setCheckState(ColTitle, Qt::Unchecked);
            }
        }
    }
    block.unblock();
    itemsTree_->viewport()->update();
    updateSaveButton();
}

// ---------- ジョブ ----------
QTreeWidgetItem* MainWindow::jobItem(const QString& id) const
{
    for (int i = 0; i < jobsTree_->topLevelItemCount(); ++i)
        if (jobsTree_->topLevelItem(i)->data(0, IdRole).toString() == id)
            return jobsTree_->topLevelItem(i);
    return nullptr;
}

void MainWindow::refreshJob(const QString& id)
{
    const auto j = jobs_.value(id);
    QTreeWidgetItem* row = jobItem(id);
    if (!j || !row)
        return;
    static const QHash<QString, QString> names = {
        {"queued", "待機中"}, {"running", "保存中"}, {"done", "完了"}, {"cancelled", "中止"}};
    QString st = names.value(j->status);
    if (j->failed)
        st += QString("（失敗 %1）").arg(j->failed);
    row->setText(1, st);
    row->setForeground(1, j->failed ? kR18 : j->status == "done" ? kSaved : palette().text().color());
    if (auto* bar = qobject_cast<QProgressBar*>(jobsTree_->itemWidget(row, 2)))
        bar->setValue(j->done + j->failed);
}

void MainWindow::cancelSelectedJobs()
{
    for (auto* row : jobsTree_->selectedItems()) {
        const QString id = row->data(0, IdRole).toString();
        if (auto j = jobs_.value(id); j && (j->status == "queued" || j->status == "running")) {
            j->cancel = true;
            if (j->status == "queued")
                j->status = "cancelled";
            refreshJob(id);
        }
    }
}

void MainWindow::clearFinishedJobs()
{
    for (int i = jobsTree_->topLevelItemCount() - 1; i >= 0; --i) {
        auto* row = jobsTree_->topLevelItem(i);
        const QString id = row->data(0, IdRole).toString();
        const auto j = jobs_.value(id);
        if (!j || j->status == "done" || j->status == "cancelled") {
            delete row;
            jobs_.remove(id);
        }
    }
}

bool MainWindow::hasActiveJobs() const
{
    return std::any_of(jobs_.begin(), jobs_.end(), [](const auto& j) { return j->status == "queued" || j->status == "running"; });
}

// ---------- ライブラリ ----------
void MainWindow::reloadLibrary()
{
    using Data = std::pair<QList<pixiv::LibNovel>, QList<wayback::LibSite>>;
    auto* w = new QFutureWatcher<Data>(this);
    connect(w, &QFutureWatcher<Data>::finished, this, [this, w] {
        std::tie(lib_, sites_) = w->result();
        w->deleteLater();
        renderLibrary();
    });
    w->setFuture(QtConcurrent::run([] { return Data{pixiv::library(), wayback::library()}; }));
}

QIcon MainWindow::coverIcon(const QString& path)
{
    if (path.isEmpty())
        return {};
    auto it = coverCache_.find(path);
    if (it == coverCache_.end()) {
        QPixmap pm(path);
        it = coverCache_.insert(path, pm.isNull() ? QIcon() : QIcon(pm.scaled(60, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    }
    return *it;
}

void MainWindow::renderLibrary()
{
    rootLabel_->setText("保存先: " + QDir::toNativeSeparators(paths::libraryRoot()));
    const QString q = searchEdit_->text().trimmed();
    auto matches = [&](const QStringList& fields) {
        return q.isEmpty() || std::any_of(fields.begin(), fields.end(), [&](const QString& s) { return s.contains(q, Qt::CaseInsensitive); });
    };
    QList<pixiv::LibNovel> list;
    std::copy_if(lib_.begin(), lib_.end(), std::back_inserter(list),
                 [&](const pixiv::LibNovel& n) { return matches(QStringList{n.title, n.userName, n.seriesTitle} + n.tags); });
    QList<wayback::LibSite> sites;
    qsizetype totalPages = 0, shownPages = 0;
    for (auto s : sites_) {
        totalPages += s.pages.size();
        if (!matches({s.host})) {
            s.pages.erase(std::remove_if(s.pages.begin(), s.pages.end(),
                                         [&](const wayback::LibPage& p) { return !matches({p.title, p.url}); }),
                          s.pages.end());
        }
        shownPages += s.pages.size();
        if (!s.pages.isEmpty())
            sites << s;
    }
    QString count = q.isEmpty() ? QString("pixiv %1話").arg(lib_.size()) : QString("pixiv %1 / %2話").arg(list.size()).arg(lib_.size());
    if (totalPages)
        count += q.isEmpty() ? QString(" ・ Wayback %1ページ").arg(totalPages) : QString(" ・ Wayback %1 / %2ページ").arg(shownPages).arg(totalPages);
    libCount_->setText(count);

    libTree_->clear();
    if (list.isEmpty() && sites.isEmpty()) {
        auto* empty = new QTreeWidgetItem(libTree_);
        empty->setText(0, lib_.isEmpty() && sites_.isEmpty() ? "まだ保存した作品はありません。「取り込み」から URL を読み込んでください" : "見つかりません");
        empty->setFlags(Qt::NoItemFlags);
        return;
    }

    // 作者 → (シリーズ or 単発) → 話。新しく更新されたものを上に
    struct Work {
        bool series = false;
        QString title, latest;
        QList<pixiv::LibNovel> eps;
    };
    struct Author {
        QString name, latest, dir;
        QList<Work> works;
    };
    QList<Author> authors;
    QHash<QString, int> authorIndex;
    for (const auto& n : list) {
        if (!authorIndex.contains(n.userId)) {
            authorIndex.insert(n.userId, int(authors.size()));
            // 作者フォルダ = pixivRoot 直下のフォルダ
            const QString root = pixiv::pixivRoot();
            const QString authorDir = root + "/" + QDir(root).relativeFilePath(n.dir).section('/', 0, 0);
            authors.append({n.userName, {}, authorDir, {}});
        }
        Author& a = authors[authorIndex[n.userId]];
        auto w = std::find_if(a.works.begin(), a.works.end(), [&](const Work& w) {
            return n.seriesId.isEmpty() ? (!w.series && w.eps.first().id == n.id) : (w.series && w.eps.first().seriesId == n.seriesId);
        });
        if (w == a.works.end()) {
            a.works.append({!n.seriesId.isEmpty(), n.seriesId.isEmpty() ? n.title : n.seriesTitle, {}, {}});
            w = a.works.end() - 1;
        }
        w->eps << n;
        w->latest = std::max(w->latest, n.uploadDate);
        a.latest = std::max(a.latest, n.uploadDate);
    }
    std::sort(authors.begin(), authors.end(), [](const Author& x, const Author& y) { return x.latest > y.latest; });

    for (auto& a : authors) {
        std::sort(a.works.begin(), a.works.end(), [](const Work& x, const Work& y) { return x.latest > y.latest; });
        qsizetype total = 0;
        for (const auto& w : a.works)
            total += w.eps.size();
        auto* ai = new QTreeWidgetItem(libTree_);
        ai->setText(LibTitle, QString("%1（%2話）").arg(a.name).arg(total));
        QFont f = ai->font(LibTitle);
        f.setBold(true);
        ai->setFont(LibTitle, f);
        ai->setData(0, KindRole, "author");
        ai->setData(0, UserRole, a.works.first().eps.first().userId);
        ai->setData(0, DirRole, a.dir);

        for (auto& w : a.works) {
            std::sort(w.eps.begin(), w.eps.end(), [](const auto& x, const auto& y) { return x.order < y.order; });
            const auto cover = std::find_if(w.eps.begin(), w.eps.end(), [](const auto& n) { return !n.cover.isEmpty(); });
            QTreeWidgetItem* parent = ai;
            qint64 chars = 0;
            for (const auto& n : w.eps)
                chars += n.textCount;
            if (w.series) {
                auto* si = new QTreeWidgetItem(ai);
                si->setText(LibTitle, QString("シリーズ「%1」（%2話）").arg(w.title).arg(w.eps.size()));
                si->setText(LibChars, num(chars) + "字");
                si->setText(LibDate, ymd(w.latest));
                si->setData(0, KindRole, "series");
                si->setData(0, IdRole, w.eps.first().seriesId);
                si->setData(0, DirRole, QFileInfo(w.eps.first().dir).path());
                si->setData(0, UserRole, w.eps.first().dir);  // 第1話
                if (cover != w.eps.end())
                    si->setIcon(LibTitle, coverIcon(cover->cover));
                parent = si;
            }
            for (const auto& n : w.eps) {
                auto* ni = new QTreeWidgetItem(parent);
                ni->setText(LibTitle, (w.series ? QString("#%1  ").arg(n.order) : QString()) + n.title + (n.r18 ? "  [R-18]" : ""));
                ni->setToolTip(LibTitle, n.title);
                ni->setText(LibChars, num(n.textCount) + "字");
                ni->setTextAlignment(LibChars, Qt::AlignRight | Qt::AlignVCenter);
                ni->setText(LibDate, ymd(n.uploadDate));
                ni->setText(LibTags, n.tags.join(" "));
                ni->setData(0, KindRole, "novel");
                ni->setData(0, IdRole, n.id);
                ni->setData(0, DirRole, n.dir);
                if (!w.series)
                    ni->setIcon(LibTitle, coverIcon(n.cover));
            }
        }
        ai->setExpanded(true);
    }

    // Wayback Machine から保存したサイト → ページ
    for (const auto& s : sites) {
        auto* si = new QTreeWidgetItem(libTree_);
        si->setText(LibTitle, QString("%1（Wayback・%2ページ）").arg(s.host).arg(s.pages.size()));
        QFont f = si->font(LibTitle);
        f.setBold(true);
        si->setFont(LibTitle, f);
        si->setData(0, KindRole, "site");
        si->setData(0, DirRole, s.dir);
        si->setData(0, UserRole, s.index);
        // geocities のように1つのホストに別々のサイトが同居していることが多いので、最初のフォルダでまとめる
        QMap<QString, QTreeWidgetItem*> folders;
        QSet<QString> firstSegs;
        for (const auto& p : s.pages)
            firstSegs.insert(QUrl(p.url).path().section('/', 1, 1, QString::SectionSkipEmpty));
        const bool split = firstSegs.size() > 1;
        for (const auto& p : s.pages) {
            QTreeWidgetItem* parent = si;
            if (split) {
                const QString path = QUrl(p.url).path();
                const QString seg = path.count('/') > 1 ? path.section('/', 1, 1) : QString();
                QTreeWidgetItem*& f = folders[seg];
                if (!f) {
                    f = new QTreeWidgetItem(si);
                    f->setData(0, KindRole, "folder");
                    f->setData(0, DirRole, s.dir + "/view/" + seg);
                }
                f->setText(LibTitle, QString("/%1（%2ページ）").arg(seg.isEmpty() ? QString() : seg + "/").arg(f->childCount() + 1));
                parent = f;
            }
            auto* pi = new QTreeWidgetItem(parent);
            pi->setText(LibTitle, p.title.isEmpty() ? QUrl(p.url).path() : p.title);
            pi->setToolTip(LibTitle, p.url);
            pi->setText(LibChars, kb(p.size));
            pi->setTextAlignment(LibChars, Qt::AlignRight | Qt::AlignVCenter);
            pi->setText(LibDate, ymd(QDateTime::fromString(p.ts, "yyyyMMddhhmmss").toString(Qt::ISODate)));
            pi->setText(LibTags, p.url);
            pi->setData(0, KindRole, "page");
            pi->setData(0, IdRole, p.url);
            pi->setData(0, UserRole, p.ts);
            pi->setData(0, DirRole, p.viewFile);
        }
        si->setExpanded(true);
        for (auto* f : std::as_const(folders))
            f->setExpanded(!q.isEmpty() || f->childCount() <= 30);
    }
    if (!q.isEmpty())
        libTree_->expandAll();
}

void MainWindow::libraryMenu(const QPoint& pos)
{
    QTreeWidgetItem* it = libTree_->itemAt(pos);
    if (!it)
        return;
    const QString kind = it->data(0, KindRole).toString(), dir = it->data(0, DirRole).toString(),
                  id = it->data(0, IdRole).toString();
    QMenu menu(this);
    if (kind == "novel") {
        menu.addAction("読む", this, [this, dir] { openNovel(dir); });
        menu.addAction("フォルダを開く", this, [this, dir] { openFolder(dir); });
        menu.addAction("pixiv で開く", this, [id] { QDesktopServices::openUrl(QUrl("https://www.pixiv.net/novel/show.php?id=" + id)); });
    } else if (kind == "series") {
        const QString first = it->data(0, UserRole).toString();
        menu.addAction("第1話から読む", this, [this, first] { openNovel(first); });
        menu.addAction("フォルダを開く", this, [this, dir] { openFolder(dir); });
        menu.addAction("pixiv で開く", this, [id] { QDesktopServices::openUrl(QUrl("https://www.pixiv.net/novel/series/" + id)); });
        menu.addAction("シリーズの更新を確認", this, [this, id] { loadUrl("https://www.pixiv.net/novel/series/" + id); });
    } else if (kind == "author") {
        const QString uid = it->data(0, UserRole).toString();
        menu.addAction("新しい話・更新を確認", this, [this, uid] { checkUpdates(uid); });
        menu.addAction("フォルダを開く", this, [this, dir] { openFolder(dir); });
        menu.addAction("pixiv で開く", this, [uid] { QDesktopServices::openUrl(QUrl("https://www.pixiv.net/users/" + uid + "/novels")); });
    } else if (kind == "site") {
        const QString index = it->data(0, UserRole).toString();
        menu.addAction("保存したページの一覧を開く", this, [index] { QDesktopServices::openUrl(QUrl::fromLocalFile(index)); });
        menu.addAction("フォルダを開く", this, [this, dir] { openFolder(dir); });
    } else if (kind == "folder") {
        menu.addAction("フォルダを開く", this, [this, dir] { openFolder(dir); });
    } else if (kind == "page") {
        const QString ts = it->data(0, UserRole).toString();
        menu.addAction("読む", this, [dir] { QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
        menu.addAction("フォルダを開く", this, [this, dir] { openFolder(QFileInfo(dir).path()); });
        menu.addAction("Wayback で開く", this, [id, ts] { QDesktopServices::openUrl(QUrl("https://web.archive.org/web/" + ts + "/" + id)); });
        menu.addAction("このページから一覧を取り直す", this, [this, id, ts] { loadUrl("https://web.archive.org/web/" + ts + "/" + id); });
    } else {
        return;
    }
    menu.exec(libTree_->viewport()->mapToGlobal(pos));
}

void MainWindow::openNovel(const QString& dir)
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir + "/novel.html"));
}

void MainWindow::openFolder(const QString& dir)
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::checkUpdates(const QString& userId) { loadUrl("https://www.pixiv.net/users/" + userId); }

// ---------- ウィンドウ ----------
void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    // Chrome で URL をコピーしてから戻ってきたら、入力欄に入れておく
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && urlEdit_->text().isEmpty()) {
        static const QRegularExpression pixivUrl(R"(^https?://(www\.)?pixiv\.net/.*(users/\d+|novel/(series/\d+|show\.php\?id=\d+)))");
        const QString clip = QApplication::clipboard()->text().trimmed();
        if (pixivUrl.match(clip).hasMatch() || (wayback::isWaybackUrl(clip) && !clip.contains('\n'))) {
            urlEdit_->setText(clip);
            urlEdit_->selectAll();
        }
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (hasActiveJobs() &&
        QMessageBox::question(this, "終了", "保存中の作業があります。中止して終了しますか？\n（保存済みの話は残ります）") != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    event->accept();
}

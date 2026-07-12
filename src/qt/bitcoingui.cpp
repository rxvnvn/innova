/*
 * Qt4 bitcoin GUI.
 *
 * W.J. van der Laan 2011-2012
 * The Bitcoin Developers 2011-2012
 */
#include "bitcoingui.h"
#include "transactiontablemodel.h"
#include "addressbookpage.h"
#include "sendcoinsdialog.h"
#include "signverifymessagedialog.h"
#include "optionsdialog.h"
#include "aboutdialog.h"
#include "clientmodel.h"
#include "walletmodel.h"
#include "editaddressdialog.h"
#include "optionsmodel.h"
#include "transactiondescdialog.h"
#include "addresstablemodel.h"
#include "transactionview.h"
#include "overviewpage.h"
#include "idagpage.h"
#include "collateralnodemanager.h"
#include "collateral.h"
#include "mintingview.h"
#include "multisigdialog.h"
#include "bitcoinunits.h"
#include "guiconstants.h"
#include "askpassphrasedialog.h"
#include "notificator.h"
#include "guiutil.h"
#include "rpcconsole.h"
#include "wallet.h"
#include "stakingpage.h"
#include "privacypage.h"
#include "nullsendpage.h"

#ifdef Q_OS_MAC
#include "macdockiconhandler.h"
#endif

#include <QApplication>
#include <QMainWindow>
#include <QMenuBar>
#include <QMenu>
#include <QIcon>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QToolBar>
#include <QStatusBar>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QStackedWidget>
#include <QDateTime>
#include <QMovie>
#include <QFileDialog>
#include <QDesktopServices>
#include <QTimer>
#include <QDragEnterEvent>
#include <QUrl>
#include <QStyle>
#include <QSizePolicy>
#include <QScreen>
#include <QSettings>
#include <QLayout>
#include <QTextDocument>
#include <QGraphicsScene>
#include <QPainter>
#include <QPalette>
#include <QPolygonF>

#include <iostream>
#include <fstream>

namespace fs = boost::filesystem;

extern CWallet* pwalletMain;
extern int64_t nLastCoinStakeSearchInterval;
double GetPoSKernelPS();


namespace {
const QSize DEFAULT_MAIN_WINDOW_SIZE(1280, 800);
const QSize MINIMUM_MAIN_WINDOW_SIZE(1024, 640);
const int MAIN_WINDOW_SCREEN_MARGIN = 32;

enum ToolbarGlyph
{
    GlyphOverview,
    GlyphSend,
    GlyphReceive,
    GlyphHistory,
    GlyphAddressBook,
    GlyphStake,
    GlyphNullSend,
    GlyphCollateralNodes,
    GlyphBlock,
    GlyphExport,
    GlyphLock,
    GlyphKey
};

static QPixmap MakeToolbarPixmap(ToolbarGlyph glyph, const QColor& primary, const QColor& accent)
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPen pen(primary, 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (glyph)
    {
    case GlyphOverview:
        painter.drawRoundedRect(QRectF(7, 8, 18, 16), 2, 2);
        painter.drawLine(QPointF(7, 14), QPointF(25, 14));
        painter.drawLine(QPointF(16, 14), QPointF(16, 24));
        break;
    case GlyphSend:
    {
        QPolygonF plane;
        plane << QPointF(6, 16) << QPointF(26, 7) << QPointF(20, 25) << QPointF(15, 18);
        painter.drawPolygon(plane);
        painter.drawLine(QPointF(15, 18), QPointF(26, 7));
        break;
    }
    case GlyphReceive:
        painter.drawLine(QPointF(16, 6), QPointF(16, 20));
        painter.drawLine(QPointF(10, 14), QPointF(16, 20));
        painter.drawLine(QPointF(22, 14), QPointF(16, 20));
        painter.drawLine(QPointF(8, 25), QPointF(24, 25));
        painter.drawLine(QPointF(8, 21), QPointF(8, 25));
        painter.drawLine(QPointF(24, 21), QPointF(24, 25));
        break;
    case GlyphHistory:
        painter.drawEllipse(QRectF(7, 7, 18, 18));
        painter.drawLine(QPointF(16, 16), QPointF(16, 10));
        painter.drawLine(QPointF(16, 16), QPointF(21, 18));
        painter.drawLine(QPointF(8, 16), QPointF(5, 16));
        break;
    case GlyphAddressBook:
        painter.drawRoundedRect(QRectF(8, 6, 17, 20), 2, 2);
        painter.drawLine(QPointF(12, 6), QPointF(12, 26));
        painter.drawEllipse(QRectF(16, 11, 5, 5));
        painter.drawArc(QRectF(14, 16, 9, 7), 0, 180 * 16);
        break;
    case GlyphStake:
        painter.drawEllipse(QRectF(9, 7, 15, 19));
        painter.drawLine(QPointF(16, 25), QPointF(20, 13));
        painter.drawLine(QPointF(16, 20), QPointF(11, 17));
        break;
    case GlyphNullSend:
    {
        QPolygonF shield;
        shield << QPointF(16, 6) << QPointF(25, 10) << QPointF(23, 22) << QPointF(16, 27) << QPointF(9, 22) << QPointF(7, 10);
        painter.drawPolygon(shield);
        painter.drawLine(QPointF(11, 16), QPointF(21, 16));
        painter.drawLine(QPointF(18, 12), QPointF(22, 16));
        painter.drawLine(QPointF(18, 20), QPointF(22, 16));
        break;
    }
    case GlyphCollateralNodes:
        painter.drawLine(QPointF(11, 12), QPointF(21, 12));
        painter.drawLine(QPointF(11, 12), QPointF(16, 23));
        painter.drawLine(QPointF(21, 12), QPointF(16, 23));
        painter.setBrush(accent);
        painter.drawEllipse(QRectF(7, 8, 8, 8));
        painter.drawEllipse(QRectF(17, 8, 8, 8));
        painter.drawEllipse(QRectF(12, 20, 8, 8));
        painter.setBrush(Qt::NoBrush);
        break;
    case GlyphBlock:
        painter.drawRoundedRect(QRectF(8, 8, 16, 16), 2, 2);
        painter.drawLine(QPointF(12, 8), QPointF(12, 24));
        painter.drawLine(QPointF(20, 8), QPointF(20, 24));
        painter.drawLine(QPointF(8, 16), QPointF(24, 16));
        break;
    case GlyphExport:
        painter.drawLine(QPointF(16, 21), QPointF(16, 7));
        painter.drawLine(QPointF(10, 13), QPointF(16, 7));
        painter.drawLine(QPointF(22, 13), QPointF(16, 7));
        painter.drawLine(QPointF(8, 24), QPointF(24, 24));
        painter.drawLine(QPointF(8, 19), QPointF(8, 24));
        painter.drawLine(QPointF(24, 19), QPointF(24, 24));
        break;
    case GlyphLock:
        painter.drawArc(QRectF(10, 7, 12, 14), 0, 180 * 16);
        painter.drawRoundedRect(QRectF(9, 15, 14, 11), 2, 2);
        painter.drawLine(QPointF(16, 19), QPointF(16, 22));
        break;
    case GlyphKey:
        painter.drawEllipse(QRectF(6, 12, 9, 9));
        painter.drawLine(QPointF(15, 16.5), QPointF(26, 16.5));
        painter.drawLine(QPointF(22, 16.5), QPointF(22, 20));
        painter.drawLine(QPointF(25, 16.5), QPointF(25, 19));
        break;
    }

    return pixmap;
}

static QIcon MakeToolbarIcon(ToolbarGlyph glyph)
{
    QIcon icon;
    icon.addPixmap(MakeToolbarPixmap(glyph, QColor("#26384f"), QColor("#1f5f99")), QIcon::Normal, QIcon::Off);
    icon.addPixmap(MakeToolbarPixmap(glyph, QColor("#0f4c81"), QColor("#1565c0")), QIcon::Normal, QIcon::On);
    icon.addPixmap(MakeToolbarPixmap(glyph, QColor("#0f4c81"), QColor("#1565c0")), QIcon::Active, QIcon::Off);
    icon.addPixmap(MakeToolbarPixmap(glyph, QColor("#0f4c81"), QColor("#1565c0")), QIcon::Selected, QIcon::On);
    icon.addPixmap(MakeToolbarPixmap(glyph, QColor("#9aa3ad"), QColor("#9aa3ad")), QIcon::Disabled, QIcon::Off);
    return icon;
}

static QPixmap MakeStatusIcon(const QString& resource, const QColor& color)
{
    QPixmap source = QIcon(resource).pixmap(STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);
    QPixmap result(source.size());
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.drawPixmap(0, 0, source);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(result.rect(), color);
    return result;
}
}

ActiveLabel::ActiveLabel(const QString & text, QWidget * parent):
    QLabel(parent){}

void ActiveLabel::mouseReleaseEvent(QMouseEvent * event)
{
    emit clicked();
}


BitcoinGUI::BitcoinGUI(QWidget *parent):
    QMainWindow(parent),
    clientModel(0),
    walletModel(0),
    encryptWalletAction(0),
    changePassphraseAction(0),
    unlockWalletAction(0),
    lockWalletAction(0),
    aboutQtAction(0),
    trayIcon(0),
    notificator(0),
    rpcConsole(0),
    nWeight(0),
    prevBlocks(0),
    spinnerFrame(0),
    nBlocksInLastPeriod(0),
    nLastBlocks(0)
{
    QSettings settings;
    const QByteArray savedGeometry = settings.value("MainWindowGeometry").toByteArray();
    QScreen *screen = QApplication::primaryScreen();
    QRect screenGeom = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    const QSize effectiveMinimumSize = MINIMUM_MAIN_WINDOW_SIZE.boundedTo(screenGeom.size());
    setMinimumSize(effectiveMinimumSize);
    if(savedGeometry.isEmpty() || !restoreGeometry(savedGeometry))
    {
        const QSize availableSize = screenGeom.size() - QSize(MAIN_WINDOW_SCREEN_MARGIN, MAIN_WINDOW_SCREEN_MARGIN);
        const QSize startSize = DEFAULT_MAIN_WINDOW_SIZE.boundedTo(availableSize).expandedTo(effectiveMinimumSize);
        resize(startSize);
        move(screenGeom.center() - rect().center());
    }

    setWindowTitle(tr("Innova") + " - " + tr("Wallet"));

#ifndef Q_OS_MAC
    qApp->setWindowIcon(QIcon(":icons/innova"));
    setWindowIcon(QIcon(":icons/innova"));
#else
    setUnifiedTitleAndToolBarOnMac(false);
    QApplication::setAttribute(Qt::AA_DontShowIconsInMenus);
#endif


    // Accept D&D of URIs
    setAcceptDrops(true);

    createActions();
    createMenuBar();
    createToolBars();
    createTrayIcon();

    fCNLock = GetBoolArg("-cnconflock");
    fNativeTor = GetBoolArg("-nativetor");
    // Create tabs
    overviewPage = new OverviewPage();
    idagPage = new IDAGPage(this);
	multisigPage = new MultisigDialog(this);
    stakingPage = new StakingPage(this);
    privacyPage = new PrivacyPage(this);
    nullsendPage = new NullSendPage(this);

    transactionsPage = new QWidget(this);
    QVBoxLayout *vbox = new QVBoxLayout();
    transactionView = new TransactionView(this);
    vbox->addWidget(transactionView);
    transactionsPage->setLayout(vbox);

	mintingPage = new QWidget(this);
    QVBoxLayout *vboxMinting = new QVBoxLayout();
    mintingView = new MintingView(this);
    vboxMinting->addWidget(mintingView);
    mintingPage->setLayout(vboxMinting);

    addressBookPage = new AddressBookPage(AddressBookPage::ForEditing, AddressBookPage::SendingTab);

    receiveCoinsPage = new AddressBookPage(AddressBookPage::ForEditing, AddressBookPage::ReceivingTab);

    sendCoinsPage = new SendCoinsDialog(this);

    collateralnodeManagerPage = new CollateralnodeManager(this);

    signVerifyMessageDialog = new SignVerifyMessageDialog(this);

    centralWidget = new QStackedWidget(this);
    centralWidget->addWidget(overviewPage);
    centralWidget->addWidget(transactionsPage);
	centralWidget->addWidget(mintingPage);
    centralWidget->addWidget(addressBookPage);
    centralWidget->addWidget(receiveCoinsPage);
    centralWidget->addWidget(sendCoinsPage);
    centralWidget->addWidget(idagPage);
    centralWidget->addWidget(collateralnodeManagerPage);
    centralWidget->addWidget(stakingPage);
    centralWidget->addWidget(privacyPage);
    centralWidget->addWidget(nullsendPage);
    setCentralWidget(centralWidget);

    // Create status bar
    statusBar();

    // Status bar notification icons
    QFrame *frameBlocks = new QFrame();
    frameBlocks->setContentsMargins(0,0,0,0);
    frameBlocks->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    QHBoxLayout *frameBlocksLayout = new QHBoxLayout(frameBlocks);
    frameBlocksLayout->setContentsMargins(3,0,3,0);
    frameBlocksLayout->setSpacing(3);
    labelEncryptionIcon = new ActiveLabel();

    labelStakingIcon = new QLabel();
    labelConnectionsIcon = new QLabel();
    labelBlocksIcon = new QLabel();
    labelConnectTypeIcon = new QLabel();
    labelCNLockIcon = new QLabel();
    frameBlocksLayout->addStretch();
    frameBlocksLayout->addWidget(labelEncryptionIcon);
    frameBlocksLayout->addStretch();
    frameBlocksLayout->addWidget(labelConnectTypeIcon);
    frameBlocksLayout->addStretch();
    if (fCNLock)
        frameBlocksLayout->addWidget(labelCNLockIcon);
        frameBlocksLayout->addStretch();
    frameBlocksLayout->addWidget(labelStakingIcon);
    frameBlocksLayout->addStretch();
    frameBlocksLayout->addWidget(labelConnectionsIcon);
    frameBlocksLayout->addStretch();
    frameBlocksLayout->addWidget(labelBlocksIcon);
    frameBlocksLayout->addStretch();

    if (GetBoolArg("-staking", true))
    {
        QTimer *timerStakingIcon = new QTimer(labelStakingIcon);
        connect(timerStakingIcon, SIGNAL(timeout()), this, SLOT(updateStakingIcon()));
        timerStakingIcon->start(30 * 1000); // Update every 30 seconds to reduce UI thread lock contention
        updateStakingIcon();
    }

    connect(labelEncryptionIcon, SIGNAL(clicked()), unlockWalletAction, SLOT(trigger()));

    // Progress bar and label for blocks download
    progressBarLabel = new QLabel();
    progressBarLabel->setVisible(false);
    progressBar = new QProgressBar();
    progressBar->setAlignment(Qt::AlignCenter);
    progressBar->setVisible(false);

    // Override style sheet for progress bar for styles that have a segmented progress bar,
    // as they make the text unreadable (workaround for issue #1071)
    // See https://qt-project.org/doc/qt-4.8/gallery.html
    QString curStyle = qApp->style()->metaObject()->className();
    if(curStyle == "QWindowsStyle" || curStyle == "QWindowsXPStyle")
    {
        progressBar->setStyleSheet("");
    }

    statusBar()->addWidget(progressBarLabel);
    statusBar()->addWidget(progressBar);
    statusBar()->addPermanentWidget(frameBlocks);

    connect(overviewPage, SIGNAL(transactionClicked(QModelIndex)), this, SLOT(gotoHistoryPage()));
    connect(overviewPage, SIGNAL(transactionClicked(QModelIndex)), transactionView, SLOT(focusTransaction(QModelIndex)));

    connect(transactionView, SIGNAL(doubleClicked(QModelIndex)), transactionView, SLOT(showDetails()));

    rpcConsole = new RPCConsole(this);
    connect(openRPCConsoleAction, SIGNAL(triggered()), rpcConsole, SLOT(show()));

    connect(addressBookPage, SIGNAL(verifyMessage(QString)), this, SLOT(gotoVerifyMessageTab(QString)));
    connect(receiveCoinsPage, SIGNAL(signMessage(QString)), this, SLOT(gotoSignMessageTab(QString)));

    gotoOverviewPage();
}

BitcoinGUI::~BitcoinGUI()
{
    QSettings settings;
    settings.setValue("MainWindowGeometry", saveGeometry());

    if(trayIcon) // Hide tray icon, as deleting will let it linger until quit (on Ubuntu)
        trayIcon->hide();
#ifdef Q_OS_MAC
    delete appMenuBar;
#endif
}

void BitcoinGUI::createActions()
{
    QActionGroup *tabGroup = new QActionGroup(this);

    overviewAction = new QAction(MakeToolbarIcon(GlyphOverview), tr("&Overview"), this);
    overviewAction->setToolTip(tr("Show general overview of wallet"));
    overviewAction->setCheckable(true);
    overviewAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_1));
	overviewAction->setStatusTip(tr("Wallet Overview"));
    tabGroup->addAction(overviewAction);

    idagAction = new QAction(MakeToolbarIcon(GlyphBlock), tr("IDA&G"), this);
    idagAction->setToolTip(tr("View IDAG consensus status"));
    idagAction->setCheckable(true);
    idagAction->setStatusTip(tr("IDAG Consensus Status"));
    tabGroup->addAction(idagAction);


    nullsendAction = new QAction(MakeToolbarIcon(GlyphNullSend), tr("&NullSend"), this);
    nullsendAction->setToolTip(tr("NullSend multi-party mixing for transaction unlinkability"));
    nullsendAction->setCheckable(true);
    nullsendAction->setStatusTip(tr("NullSend Mixing"));
    tabGroup->addAction(nullsendAction);

    sendCoinsAction = new QAction(MakeToolbarIcon(GlyphSend), tr("&Send"), this);
    sendCoinsAction->setToolTip(tr("Send coins to an Innova address"));
    sendCoinsAction->setCheckable(true);
	sendCoinsAction->setStatusTip(tr("Send Innova"));
    sendCoinsAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_2));
    tabGroup->addAction(sendCoinsAction);

    receiveCoinsAction = new QAction(MakeToolbarIcon(GlyphReceive), tr("&Receive"), this);
    receiveCoinsAction->setToolTip(tr("Show the list of addresses for receiving payments"));
    receiveCoinsAction->setCheckable(true);
	receiveCoinsAction->setStatusTip(tr("Receive Innova"));
    receiveCoinsAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_3));
    tabGroup->addAction(receiveCoinsAction);

    historyAction = new QAction(MakeToolbarIcon(GlyphHistory), tr("&Transactions"), this);
    historyAction->setToolTip(tr("Browse transaction history"));
    historyAction->setCheckable(true);
	historyAction->setStatusTip(tr("Transactions"));
    historyAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_4));
    tabGroup->addAction(historyAction);

    addressBookAction = new QAction(MakeToolbarIcon(GlyphAddressBook), tr("&Address Book"), this);
    addressBookAction->setToolTip(tr("Edit the list of stored addresses and labels"));
    addressBookAction->setCheckable(true);
	addressBookAction->setStatusTip(tr("Address Book"));
    addressBookAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_5));
    tabGroup->addAction(addressBookAction);

	mintingAction = new QAction(MakeToolbarIcon(GlyphStake), tr("Staking &Inputs"), this);
    mintingAction->setToolTip(tr("View staking inputs and estimated earnings"));
    mintingAction->setCheckable(true);
	mintingAction->setStatusTip(tr("Staking Inputs & Estimations"));
    mintingAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_9));
    tabGroup->addAction(mintingAction);

    collateralnodeManagerAction = new QAction(MakeToolbarIcon(GlyphCollateralNodes), tr("&Collateral Nodes"), this);
    collateralnodeManagerAction->setToolTip(tr("Show Innova Collateral Nodes status and configure your nodes."));
    collateralnodeManagerAction->setCheckable(true);
	collateralnodeManagerAction->setStatusTip(tr("Collateral Nodes"));
    tabGroup->addAction(collateralnodeManagerAction);

    stakingAction = new QAction(MakeToolbarIcon(GlyphStake), tr("Sta&king"), this);
    stakingAction->setToolTip(tr("Manage staking mode, cold staking, and privacy staking"));
    stakingAction->setCheckable(true);
    stakingAction->setStatusTip(tr("Staking Control Panel"));
    tabGroup->addAction(stakingAction);

    // Privacy page removed — entire chain is private; address generation integrated into Receive page
    privacyAction = new QAction(QIcon(":/icons/lock_closed"), tr("&Privacy"), this);
    privacyAction->setCheckable(true);
    privacyAction->setVisible(false); // Hidden — not needed as separate page

	multisigAction = new QAction(QIcon(":/icons/multi"), tr("M&ultisig"), this);
    tabGroup->addAction(multisigAction);
	multisigAction->setStatusTip(tr("Multisig Interface"));

    connect(overviewAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(overviewAction, SIGNAL(triggered()), this, SLOT(gotoOverviewPage()));
    connect(idagAction, SIGNAL(triggered()), this, SLOT(gotoIDAGPage()));
    connect(sendCoinsAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(sendCoinsAction, SIGNAL(triggered()), this, SLOT(gotoSendCoinsPage()));
    connect(receiveCoinsAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(receiveCoinsAction, SIGNAL(triggered()), this, SLOT(gotoReceiveCoinsPage()));
	connect(mintingAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(mintingAction, SIGNAL(triggered()), this, SLOT(gotoMintingPage()));
    connect(collateralnodeManagerAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(collateralnodeManagerAction, SIGNAL(triggered()), this, SLOT(gotoCollateralnodeManagerPage()));
    connect(historyAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(historyAction, SIGNAL(triggered()), this, SLOT(gotoHistoryPage()));
    connect(addressBookAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(addressBookAction, SIGNAL(triggered()), this, SLOT(gotoAddressBookPage()));
	connect(multisigAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(multisigAction, SIGNAL(triggered()), this, SLOT(gotoMultisigPage()));
    connect(stakingAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(stakingAction, SIGNAL(triggered()), this, SLOT(gotoStakingPage()));
    connect(privacyAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(privacyAction, SIGNAL(triggered()), this, SLOT(gotoPrivacyPage()));
    connect(nullsendAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(nullsendAction, SIGNAL(triggered()), this, SLOT(gotoNullSendPage()));

    quitAction = new QAction(QIcon(":/icons/quit"), tr("E&xit"), this);
    quitAction->setToolTip(tr("Quit application"));
    quitAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Q));
    quitAction->setMenuRole(QAction::QuitRole);
    aboutAction = new QAction(QIcon(":/icons/bitcoin"), tr("&About Innova"), this);
    aboutAction->setToolTip(tr("Show information about Innova"));
    aboutAction->setMenuRole(QAction::AboutRole);
    aboutQtAction = new QAction(QIcon(":/trolltech/qmessagebox/images/qtlogo-64.png"), tr("About &Qt"), this);
    aboutQtAction->setToolTip(tr("Show information about Qt"));
    aboutQtAction->setMenuRole(QAction::AboutQtRole);
    optionsAction = new QAction(QIcon(":/icons/options"), tr("&Options..."), this);
    optionsAction->setToolTip(tr("Modify configuration options for Innova"));
    optionsAction->setMenuRole(QAction::PreferencesRole);
    toggleHideAction = new QAction(QIcon(":/icons/bitcoin"), tr("&Show / Hide"), this);
    encryptWalletAction = new QAction(MakeToolbarIcon(GlyphLock), tr("&Encrypt Wallet..."), this);
    encryptWalletAction->setToolTip(tr("Encrypt or decrypt wallet"));
	encryptWalletAction->setStatusTip(tr("Encrypt wallet"));
    encryptWalletAction->setCheckable(true);
    backupWalletAction = new QAction(QIcon(":/icons/filesave"), tr("&Backup Wallet..."), this);
    backupWalletAction->setToolTip(tr("Backup wallet to another location"));
    changePassphraseAction = new QAction(MakeToolbarIcon(GlyphKey), tr("&Change Passphrase..."), this);
    changePassphraseAction->setToolTip(tr("Change the passphrase used for wallet encryption"));
	changePassphraseAction->setStatusTip(tr("Change your passphrase"));
    unlockWalletAction = new QAction(QIcon(":/icons/lock_open"), tr("&Unlock Wallet..."), this);
    unlockWalletAction->setToolTip(tr("Unlock wallet"));
	unlockWalletAction->setStatusTip(tr("Unlock wallet"));
    lockWalletAction = new QAction(QIcon(":/icons/lock_closed"), tr("&Lock Wallet"), this);
    lockWalletAction->setToolTip(tr("Lock wallet"));
	lockWalletAction->setStatusTip(tr("Lock wallet"));
    signMessageAction = new QAction(QIcon(":/icons/edit"), tr("Sign &message..."), this);
    verifyMessageAction = new QAction(QIcon(":/icons/transaction_0"), tr("&Verify message..."), this);

    exportAction = new QAction(MakeToolbarIcon(GlyphExport), tr("&Export..."), this);
    exportAction->setToolTip(tr("Export the data in the current tab to a file"));
	exportAction->setStatusTip(tr("Export the data to a file"));
    openRPCConsoleAction = new QAction(QIcon(":/icons/debugwindow"), tr("&Debug window"), this);
    openRPCConsoleAction->setToolTip(tr("Open debugging and diagnostic console"));
	openRPCConsoleAction->setStatusTip(tr("Show Debug Console"));

	openInfoAction = new QAction(QApplication::style()->standardIcon(QStyle::SP_MessageBoxInformation), tr("Node Information"), this);
    openInfoAction->setStatusTip(tr("Show diagnostic information"));
    openGraphAction = new QAction(QIcon(":/icons/connect_4"), tr("Network &Monitor"), this);
    openGraphAction->setStatusTip(tr("Show Network Monitor"));
    openPeerAction = new QAction(QIcon(":/icons/connect_4"), tr("&Peers"), this);
    openPeerAction->setStatusTip(tr("Show Innova network peers"));
    openConfEditorAction = new QAction(QIcon(":/icons/edit"), tr("Open &Wallet Configuration File"), this);
    openConfEditorAction->setStatusTip(tr("Open configuration file"));
    openMNConfEditorAction = new QAction(QIcon(":/icons/edit"), tr("Open Collateralnode Configuration &File"), this);
    openMNConfEditorAction->setStatusTip(tr("Open Collateralnode configuration file"));

    connect(quitAction, SIGNAL(triggered()), qApp, SLOT(quit()));
    connect(aboutAction, SIGNAL(triggered()), this, SLOT(aboutClicked()));
    connect(aboutQtAction, SIGNAL(triggered()), qApp, SLOT(aboutQt()));
    connect(optionsAction, SIGNAL(triggered()), this, SLOT(optionsClicked()));
    connect(toggleHideAction, SIGNAL(triggered()), this, SLOT(toggleHidden()));
    connect(encryptWalletAction, SIGNAL(triggered(bool)), this, SLOT(encryptWallet(bool)));
    connect(backupWalletAction, SIGNAL(triggered()), this, SLOT(backupWallet()));
    connect(changePassphraseAction, SIGNAL(triggered()), this, SLOT(changePassphrase()));
    connect(unlockWalletAction, SIGNAL(triggered()), this, SLOT(unlockWallet()));
    connect(lockWalletAction, SIGNAL(triggered()), this, SLOT(lockWallet()));
    connect(signMessageAction, SIGNAL(triggered()), this, SLOT(gotoSignMessageTab()));
    connect(verifyMessageAction, SIGNAL(triggered()), this, SLOT(gotoVerifyMessageTab()));

	// Jump directly to tabs in RPC-console
    connect(openInfoAction, SIGNAL(triggered()), this, SLOT(showInfo()));
    connect(openRPCConsoleAction, SIGNAL(triggered()), this, SLOT(showConsole()));
    connect(openGraphAction, SIGNAL(triggered()), this, SLOT(showGraph()));
    connect(openPeerAction, SIGNAL(triggered()), this, SLOT(showPeer()));

    // Open configs from menu
    connect(openConfEditorAction, SIGNAL(triggered()), this, SLOT(showConfEditor()));
    connect(openMNConfEditorAction, SIGNAL(triggered()), this, SLOT(showMNConfEditor()));

}

void BitcoinGUI::createMenuBar()
{
#ifdef Q_OS_MAC
    // Create a decoupled menu bar on Mac which stays even if the window is closed
    appMenuBar = new QMenuBar();
#else
    // Get the main window's menu bar on other platforms
    appMenuBar = menuBar();
#endif

    // Configure the menus
    QMenu *file = appMenuBar->addMenu(tr("&File"));
    file->addAction(backupWalletAction);
    file->addAction(exportAction);
    file->addSeparator();
    file->addAction(signMessageAction);
    file->addAction(verifyMessageAction);
    file->addSeparator();
    file->addAction(quitAction);

    QMenu *settings = appMenuBar->addMenu(tr("&Settings"));
    settings->addAction(encryptWalletAction);
    settings->addAction(changePassphraseAction);
    settings->addAction(unlockWalletAction);
    settings->addAction(lockWalletAction);
    settings->addSeparator();
    settings->addAction(openConfEditorAction);
    settings->addAction(openMNConfEditorAction);
    settings->addSeparator();
    settings->addAction(optionsAction);

    QMenu *window = appMenuBar->addMenu(tr("&Window"));
    window->addAction(overviewAction);
    window->addAction(sendCoinsAction);
    window->addAction(receiveCoinsAction);
    window->addAction(historyAction);
    window->addSeparator();
    window->addAction(addressBookAction);
    window->addAction(stakingAction);
    window->addAction(mintingAction);
    window->addAction(collateralnodeManagerAction);
    window->addAction(idagAction);
    window->addAction(nullsendAction);
    window->addAction(multisigAction);
    window->addSeparator();
    window->addAction(openInfoAction);
    window->addAction(openRPCConsoleAction);
    window->addAction(openGraphAction);
    window->addAction(openPeerAction);

    QMenu *help = appMenuBar->addMenu(tr("&Help"));
    help->addAction(aboutAction);
    help->addAction(aboutQtAction);
}

void BitcoinGUI::createToolBars()
{
    mainToolbar = addToolBar(tr("Tabs toolbar"));
    mainToolbar->setObjectName("mainToolbar");
    mainToolbar->setMovable(false);
    mainToolbar->setFloatable(false);
    mainToolbar->setIconSize(QSize(20, 20));
    mainToolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mainToolbar->setContentsMargins(4, 1, 4, 1);
    mainToolbar->layout()->setSpacing(2);

    mainToolbar->addAction(overviewAction);
    mainToolbar->addAction(sendCoinsAction);
    mainToolbar->addAction(receiveCoinsAction);
    mainToolbar->addAction(historyAction);
    overviewAction->setChecked(true);
}

void BitcoinGUI::setClientModel(ClientModel *clientModel)
{
    this->clientModel = clientModel;
    if(clientModel)
    {
        // Replace some strings and icons, when using the testnet
        if(clientModel->isTestNet())
        {
            setWindowTitle(windowTitle() + QString(" ") + tr("[testnet]"));
#ifndef Q_OS_MAC
            qApp->setWindowIcon(QIcon(":icons/bitcoin_testnet"));
            setWindowIcon(QIcon(":icons/bitcoin_testnet"));
#else
            MacDockIconHandler::instance()->setIcon(QIcon(":icons/bitcoin_testnet"));
#endif
            if(trayIcon)
            {
                trayIcon->setToolTip(tr("Innova client") + QString(" ") + tr("[testnet]"));
                trayIcon->setIcon(QIcon(":/icons/toolbar_testnet"));
                toggleHideAction->setIcon(QIcon(":/icons/toolbar_testnet"));
            }

            aboutAction->setIcon(QIcon(":/icons/toolbar_testnet"));
        }

        // Keep up to date with client
        setNumConnections(clientModel->getNumConnections());
        connect(clientModel, SIGNAL(numConnectionsChanged(int)), this, SLOT(setNumConnections(int)));


        nClientUpdateTime = GetTime();
        setNumBlocks(clientModel->getNumBlocks(), clientModel->getNumBlocksOfPeers());
        connect(clientModel, SIGNAL(numBlocksChanged(int,int)), this, SLOT(setNumBlocks(int,int)));

        connect(clientModel, SIGNAL(error(QString,QString,bool)), this, SLOT(error(QString,QString,bool)));

        rpcConsole->setClientModel(clientModel);
        idagPage->setModel(clientModel);
        addressBookPage->setOptionsModel(clientModel->getOptionsModel());
        receiveCoinsPage->setOptionsModel(clientModel->getOptionsModel());
    }

}

void BitcoinGUI::setWalletModel(WalletModel *walletModel)
{
    this->walletModel = walletModel;
    if(walletModel)
    {
        // Report errors from wallet thread
        connect(walletModel, SIGNAL(error(QString,QString,bool)), this, SLOT(error(QString,QString,bool)));

        // Put transaction list in tabs
        transactionView->setModel(walletModel);
        mintingView->setModel(walletModel);

        overviewPage->setModel(walletModel);
        addressBookPage->setModel(walletModel->getAddressTableModel());
        receiveCoinsPage->setModel(walletModel->getAddressTableModel());
        receiveCoinsPage->setWalletModel(walletModel);
        sendCoinsPage->setModel(walletModel);
        signVerifyMessageDialog->setModel(walletModel);
        collateralnodeManagerPage->setWalletModel(walletModel);
		multisigPage->setModel(walletModel);
        stakingPage->setModel(walletModel);
        privacyPage->setModel(walletModel);
        qobject_cast<NullSendPage*>(nullsendPage)->setModel(walletModel);

        setEncryptionStatus(walletModel->getEncryptionStatus());
        connect(walletModel, SIGNAL(encryptionStatusChanged(int)), this, SLOT(setEncryptionStatus(int)));

        // Balloon pop-up for new transaction
        connect(walletModel->getTransactionTableModel(), SIGNAL(rowsInserted(QModelIndex,int,int)),
                this, SLOT(incomingTransaction(QModelIndex,int,int)));

        // Ask for passphrase if needed
        connect(walletModel, SIGNAL(requireUnlock()), this, SLOT(unlockWallet()));
    }
}

void BitcoinGUI::createTrayIcon()
{
    QMenu *trayIconMenu;
#ifndef Q_OS_MAC
    trayIcon = new QSystemTrayIcon(this);
    trayIconMenu = new QMenu(this);
    trayIcon->setContextMenu(trayIconMenu);
    trayIcon->setToolTip(tr("Innova client"));
    trayIcon->setIcon(QIcon(":/icons/toolbar"));
    connect(trayIcon, SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
            this, SLOT(trayIconActivated(QSystemTrayIcon::ActivationReason)));
    trayIcon->show();
#else
    // Note: On Mac, the dock icon is used to provide the tray's functionality.
    MacDockIconHandler *dockIconHandler = MacDockIconHandler::instance();
    dockIconHandler->setMainWindow((QMainWindow *)this);
    trayIconMenu = dockIconHandler->dockMenu();
#endif

    trayIconMenu->addAction(toggleHideAction);
    trayIconMenu->addSeparator();
    trayIconMenu->addAction(sendCoinsAction);
    trayIconMenu->addAction(receiveCoinsAction);
	trayIconMenu->addSeparator();
	trayIconMenu->addAction(multisigAction);
    trayIconMenu->addSeparator();
    trayIconMenu->addAction(signMessageAction);
    trayIconMenu->addAction(verifyMessageAction);
    trayIconMenu->addSeparator();
    trayIconMenu->addAction(optionsAction);
    trayIconMenu->addAction(openRPCConsoleAction);
#ifndef Q_OS_MAC // This is built-in on Mac
    trayIconMenu->addSeparator();
    trayIconMenu->addAction(quitAction);
#endif

    notificator = new Notificator(qApp->applicationName(), trayIcon, this);
}

#ifndef Q_OS_MAC
void BitcoinGUI::trayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
    if(reason == QSystemTrayIcon::Trigger)
    {
        toggleHideAction->trigger();
    }
}
#endif

void BitcoinGUI::optionsClicked()
{
    if(!clientModel || !clientModel->getOptionsModel())
        return;
    OptionsDialog dlg;
    dlg.setModel(clientModel->getOptionsModel());
    dlg.exec();
}

void BitcoinGUI::aboutClicked()
{
    AboutDialog dlg;
    dlg.setModel(clientModel);
    dlg.exec();
}

void BitcoinGUI::setNumConnections(int count)
{
    QString icon;
    switch(count)
    {
    case 0: icon = ":/icons/connect_0"; break;
    case 1: case 2: case 3: icon = ":/icons/connect_1"; break;
    case 4: case 5: case 6: icon = ":/icons/connect_2"; break;
    case 7: case 8: case 9: icon = ":/icons/connect_3"; break;
    default: icon = ":/icons/connect_4"; break;
    }
    labelConnectionsIcon->setPixmap(MakeStatusIcon(icon, statusBar()->palette().color(QPalette::WindowText)));
    labelConnectionsIcon->setToolTip(tr("%n active connection(s) to Innova network", "", count));

    if(fNativeTor)
    {
        labelConnectTypeIcon->setPixmap(MakeStatusIcon(":/icons/tor", statusBar()->palette().color(QPalette::Highlight)));

        string automatic_onion;
        fs::path const hostname_path = GetDefaultDataDir() / "onion" / "hostname";
        if (!fs::exists(hostname_path)) {
            printf("No external address found.");
        }
        ifstream file(hostname_path.string().c_str());
        file >> automatic_onion;

        QString onionauto;
        onionauto = tr("Connected via the Tor Network - ") + QString::fromStdString(automatic_onion);
        labelConnectTypeIcon->setToolTip(onionauto);
    } else {
        labelConnectTypeIcon->setPixmap(MakeStatusIcon(":/icons/toroff", statusBar()->palette().color(QPalette::WindowText)));
        labelConnectTypeIcon->setToolTip(tr("Not Connected via the Tor Network, Start Innova with the flag nativetor=1"));
    }
    if (fCNLock == true) {
        labelCNLockIcon->setPixmap(MakeStatusIcon(":/icons/cn", statusBar()->palette().color(QPalette::Highlight)));
    }
}

void BitcoinGUI::setNumBlocks(int count, int nTotalBlocks)
{
    // don't bother showing anything if we have no connection to the network
    if (!clientModel || clientModel->getNumConnections() == 0)
    {
        progressBarLabel->setText(tr("Connecting to the Innova network..."));
        progressBarLabel->setVisible(true);
        progressBar->setVisible(false);
        return;
    }

    QString strStatusBarWarnings = clientModel->getStatusBarWarnings();
    QString tooltip;
    QString nRemainingTime;

    if (nLastBlocks == 0)
        nLastBlocks = pindexBest->nHeight;

        if (count > nLastBlocks && GetTime() - nClientUpdateTime > BPS_PERIOD) {
            nBlocksInLastPeriod = count - nLastBlocks;
            nLastBlocks = count;
            nClientUpdateTime = GetTime();
        }
        if (nBlocksInLastPeriod>0)
            nBlocksPerSec = nBlocksInLastPeriod / BPS_PERIOD;
        else
            nBlocksPerSec = 0;

            if (nBlocksPerSec>0) {
              nRemainingTime = QDateTime::fromTime_t((nTotalBlocks - count) / nBlocksPerSec).toUTC().toString("hh'h'mm'm'");
          }

          QDateTime lastBlockDate = clientModel->getLastBlockDate();
          int secs = lastBlockDate.secsTo(QDateTime::currentDateTime());
          QString text;
          if(secs <= 0)
          {
              // Fully up to date. Leave text empty.
          }
          else if(secs < 60)
          {
              text = tr("%n second(s) ago","",secs);
          }
          else if(secs < 60*60)
          {
              text = tr("%n minute(s) ago","",secs/60);
          }
          else if(secs < 24*60*60)
          {
              text = tr("%n hour(s) ago","",secs/(60*60));
          }
          else
          {
              text = tr("%n day(s) ago","",secs/(60*60*24));
          }

          if (IsInitialBlockDownload() || count < nTotalBlocks-30) // if we're in initial download or more than 30 blocks behind
          {
              int nRemainingBlocks = nTotalBlocks - count;
              float nPercentageDone = count / (nTotalBlocks * 0.01f);
              if (strStatusBarWarnings.isEmpty())
        {
            progressBarLabel->setText(tr("Synchronizing with the network..."));
            progressBarLabel->setVisible(true);
            if (nBlocksPerSec>0)
                progressBar->setFormat(tr("~%1 block(s) remaining (est: %2 at %3 blocks/sec)").arg(nRemainingBlocks).arg(nRemainingTime).arg(nBlocksPerSec));
            else
                progressBar->setFormat(tr("~%1 block(s) remaining").arg(nRemainingBlocks));
            progressBar->setMaximum(nTotalBlocks);
            progressBar->setValue(count);
            progressBar->setVisible(true);
        }

        tooltip = tr("Catching up...") + QString("<br>") + tooltip;

        if (GetTimeMicros() > nLastUpdateTime + 27000) { // 27ms per spinner frame = 1 sec per 'spin'
            labelBlocksIcon->setPixmap(MakeStatusIcon(QString(":/movies/res/movies/spinner-%1.png").arg(spinnerFrame, 3, 10, QChar('0')), statusBar()->palette().color(QPalette::Highlight)));
            spinnerFrame = (spinnerFrame + 1) % 36;
            nLastUpdateTime = GetTimeMicros();
        }

        overviewPage->showOutOfSyncWarning(true);

        tooltip = tr("Downloaded %1 of %2 blocks of transaction history (%3% done).").arg(count).arg(nTotalBlocks).arg(nPercentageDone, 0, 'f', 2);
    }
    else
    {
        if (strStatusBarWarnings.isEmpty())
            progressBarLabel->setVisible(false);

            tooltip = tr("Up to date") + QString(".<br>") + tooltip;
            labelBlocksIcon->setPixmap(MakeStatusIcon(":/icons/synced", statusBar()->palette().color(QPalette::Highlight)));
            overviewPage->showOutOfSyncWarning(false);
            progressBar->setVisible(false);
            tooltip = tr("Downloaded %1 blocks of transaction history.").arg(count);
        }

        if (!strStatusBarWarnings.isEmpty())
        {
            progressBarLabel->setText(strStatusBarWarnings);
            progressBarLabel->setVisible(true);
            progressBar->setVisible(false);
        }

        ClientModel::DAGStatus dagStatus = clientModel->getDAGStatus();
        tooltip += QString("<br>");
        if (dagStatus.lockBusy)
        {
            tooltip += tr("IDAG: updating...");
        }
        else if (dagStatus.valid)
        {
            if (!dagStatus.active)
            {
                tooltip += tr("IDAG: not active (fork height %1)").arg(dagStatus.dagForkHeight);
            }
            else
            {
                tooltip += tr("IDAG: %1 active").arg(dagStatus.orderingAlgorithm);
                tooltip += QString("<br>") + tr("DAG tips: %1").arg(dagStatus.tipCount);
                tooltip += QString("<br>") + tr("Inferred k: %1").arg(
                    dagStatus.dagKnightActive
                        ? (dagStatus.inferredKError ? tr("unavailable") : QString::number(dagStatus.inferredK))
                        : tr("N/A"));
                tooltip += QString("<br>") + tr("Adaptive block limit: %1 bytes").arg(dagStatus.adaptiveBlockLimit);
            }
        }

        if(!text.isEmpty())
        {
            tooltip += QString("<br>");
            tooltip += tr("Last received block was generated %1.").arg(text);
        }

        // Don't word-wrap this (fixed-width) tooltip
        tooltip = QString("<nobr>") + tooltip + QString("</nobr>");

        labelBlocksIcon->setToolTip(tooltip);
        progressBarLabel->setToolTip(tooltip);
        progressBar->setToolTip(tooltip);
}

void BitcoinGUI::error(const QString &title, const QString &message, bool modal)
{
    if(modal)
    {
        QMessageBox::critical(this, title, message, QMessageBox::Ok, QMessageBox::Ok);
    } else {
        notificator->notify(Notificator::Critical, title, message);
    }
}

void BitcoinGUI::changeEvent(QEvent *e)
{
    QMainWindow::changeEvent(e);
#ifndef Q_OS_MAC // Ignored on Mac
    if(e->type() == QEvent::WindowStateChange)
    {
        if(clientModel && clientModel->getOptionsModel()->getMinimizeToTray())
        {
            QWindowStateChangeEvent *wsevt = static_cast<QWindowStateChangeEvent*>(e);
            if(!(wsevt->oldState() & Qt::WindowMinimized) && isMinimized())
            {
                QTimer::singleShot(0, this, SLOT(hide()));
                e->ignore();
            }
        }
    }
#endif
}

void BitcoinGUI::closeEvent(QCloseEvent *event)
{
    if(clientModel)
    {
#ifndef Q_OS_MAC // Ignored on Mac
        if(!clientModel->getOptionsModel()->getMinimizeToTray() &&
           !clientModel->getOptionsModel()->getMinimizeOnClose())
        {
            qApp->quit();
        }
#endif
    }
    QMainWindow::closeEvent(event);
}

void BitcoinGUI::askFee(qint64 nFeeRequired, bool *payFee)
{
    QString strMessage =
        tr("This transaction is over the size limit.  You can still send it for a fee of %1, "
          "which goes to the nodes that process your transaction and helps to support the network.  "
          "Do you want to pay the fee?").arg(
                BitcoinUnits::formatWithUnit(BitcoinUnits::BTC, nFeeRequired));
    QMessageBox::StandardButton retval = QMessageBox::question(
          this, tr("Confirm transaction fee"), strMessage,
          QMessageBox::Yes|QMessageBox::Cancel, QMessageBox::Yes);
    *payFee = (retval == QMessageBox::Yes);
}

void BitcoinGUI::incomingTransaction(const QModelIndex & parent, int start, int end)
{
    if(!walletModel || !clientModel)
        return;
    TransactionTableModel *ttm = walletModel->getTransactionTableModel();
    qint64 amount = ttm->index(start, TransactionTableModel::Amount, parent)
                    .data(Qt::EditRole).toULongLong();
    if(!clientModel->inInitialBlockDownload())
    {
        QString date = ttm->index(start, TransactionTableModel::Date, parent)
                        .data().toString();
        QString type = ttm->index(start, TransactionTableModel::Type, parent)
                        .data().toString();
        QString address = ttm->index(start, TransactionTableModel::ToAddress, parent)
                        .data().toString();
        QIcon icon = qvariant_cast<QIcon>(ttm->index(start,
                            TransactionTableModel::ToAddress, parent)
                        .data(Qt::DecorationRole));

        notificator->notify(Notificator::Information,
                            (amount)<0 ? tr("Sent transaction") :
                                         tr("Incoming transaction"),
                              tr("Date: %1\n"
                                 "Amount: %2\n"
                                 "Type: %3\n"
                                 "Address: %4\n")
                              .arg(date)
                              .arg(BitcoinUnits::formatWithUnit(walletModel->getOptionsModel()->getDisplayUnit(), amount, true))
                              .arg(type)
                              .arg(address), icon);
    }
}

void BitcoinGUI::showDebugWindow()
{
    rpcConsole->showNormal();
    rpcConsole->show();
    rpcConsole->raise();
    rpcConsole->activateWindow();
}

void BitcoinGUI::showInfo()
{
    rpcConsole->setTabFocus(RPCConsole::TAB_INFO);
    showDebugWindow();
}

void BitcoinGUI::showConsole()
{
    rpcConsole->setTabFocus(RPCConsole::TAB_CONSOLE);
    showDebugWindow();
}

void BitcoinGUI::showGraph()
{
    rpcConsole->setTabFocus(RPCConsole::TAB_GRAPH);
    showDebugWindow();
}

void BitcoinGUI::showPeer()
{
    rpcConsole->setTabFocus(RPCConsole::TAB_PEER);
    showDebugWindow();
}

void BitcoinGUI::showConfEditor()
{
    boost::filesystem::path pathConfig = GetConfigFile();

    /* Open innova.conf with the associated application */
    if (boost::filesystem::exists(pathConfig)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(pathConfig.string())));
	} else {
		QMessageBox::warning(this, tr("No innova.conf"),
        tr("Your innova.conf does not exist! Please create one in your Innova data directory."),
        QMessageBox::Ok, QMessageBox::Ok);
	}
	//GUIUtil::openConfigfile();

}

void BitcoinGUI::showMNConfEditor()
{
    boost::filesystem::path pathMNConfig = GetCollateralnodeConfigFile();

    /* Open collateralnode.conf with the associated application */
    if (boost::filesystem::exists(pathMNConfig)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(pathMNConfig.string())));
	} else {
		QMessageBox::warning(this, tr("No collateralnode.conf"),
        tr("Your collateralnode.conf does not exist! Please create one in your Innova data directory."),
        QMessageBox::Ok, QMessageBox::Ok);
	}
    //GUIUtil::openMNConfigfile();
}

void BitcoinGUI::gotoOverviewPage()
{
    overviewAction->setChecked(true);
    centralWidget->setCurrentWidget(overviewPage);

    exportAction->setEnabled(false);
    disconnect(exportAction, SIGNAL(triggered()), 0, 0);
}


void BitcoinGUI::gotoStakingPage()
{
    stakingAction->setChecked(true);
    centralWidget->setCurrentWidget(stakingPage);

    exportAction->setEnabled(false);
    disconnect(exportAction, SIGNAL(triggered()), 0, 0);
}

void BitcoinGUI::gotoPrivacyPage()
{
    privacyAction->setChecked(true);
    centralWidget->setCurrentWidget(privacyPage);

    exportAction->setEnabled(false);
    disconnect(exportAction, SIGNAL(triggered()), 0, 0);
}

void BitcoinGUI::gotoNullSendPage()
{
    nullsendAction->setChecked(true);
    centralWidget->setCurrentWidget(nullsendPage);

    exportAction->setEnabled(false);
    disconnect(exportAction, SIGNAL(triggered()), 0, 0);
}

void BitcoinGUI::gotoCollateralnodeManagerPage()
{
    collateralnodeManagerAction->setChecked(true);
    centralWidget->setCurrentWidget(collateralnodeManagerPage);

    exportAction->setEnabled(false);
    disconnect(exportAction, SIGNAL(triggered()), 0, 0);
}

void BitcoinGUI::gotoMultisigPage()
{
    multisigPage->show();
    multisigPage->setFocus();
}

void BitcoinGUI::gotoMintingPage()
{
    mintingAction->setChecked(true);
    centralWidget->setCurrentWidget(mintingPage);

    exportAction->setEnabled(true);
    disconnect(exportAction, SIGNAL(triggered()), 0, 0);
    connect(exportAction, SIGNAL(triggered()), mintingView, SLOT(exportClicked()));
}

void BitcoinGUI::gotoIDAGPage()
{
    idagAction->setChecked(true);
    centralWidget->setCurrentWidget(idagPage);

    exportAction->setEnabled(false);
    disconnect(exportAction, SIGNAL(triggered()), 0, 0);
}

void BitcoinGUI::gotoHistoryPage()
{
    historyAction->setChecked(true);
    centralWidget->setCurrentWidget(transactionsPage);

    exportAction->setEnabled(true);
    disconnect(exportAction, SIGNAL(triggered()), 0, 0);
    connect(exportAction, SIGNAL(triggered()), transactionView, SLOT(exportClicked()));
}

void BitcoinGUI::gotoAddressBookPage()
{
    addressBookAction->setChecked(true);
    centralWidget->setCurrentWidget(addressBookPage);

    exportAction->setEnabled(true);
    disconnect(exportAction, SIGNAL(triggered()), 0, 0);
    connect(exportAction, SIGNAL(triggered()), addressBookPage, SLOT(exportClicked()));
}

void BitcoinGUI::gotoReceiveCoinsPage()
{
    receiveCoinsAction->setChecked(true);
    centralWidget->setCurrentWidget(receiveCoinsPage);

    exportAction->setEnabled(true);
    disconnect(exportAction, SIGNAL(triggered()), 0, 0);
    connect(exportAction, SIGNAL(triggered()), receiveCoinsPage, SLOT(exportClicked()));
}

void BitcoinGUI::gotoSendCoinsPage()
{
    sendCoinsAction->setChecked(true);
    centralWidget->setCurrentWidget(sendCoinsPage);

    exportAction->setEnabled(false);
    disconnect(exportAction, SIGNAL(triggered()), 0, 0);
}

void BitcoinGUI::gotoSignMessageTab(QString addr)
{
    // call show() in showTab_SM()
    signVerifyMessageDialog->showTab_SM(true);

    if(!addr.isEmpty())
        signVerifyMessageDialog->setAddress_SM(addr);
}

void BitcoinGUI::gotoVerifyMessageTab(QString addr)
{
    // call show() in showTab_VM()
    signVerifyMessageDialog->showTab_VM(true);

    if(!addr.isEmpty())
        signVerifyMessageDialog->setAddress_VM(addr);
}

void BitcoinGUI::dragEnterEvent(QDragEnterEvent *event)
{
    // Accept only URIs
    if(event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void BitcoinGUI::dropEvent(QDropEvent *event)
{
    if(event->mimeData()->hasUrls())
    {
        int nValidUrisFound = 0;
        QList<QUrl> uris = event->mimeData()->urls();
        foreach(const QUrl &uri, uris)
        {
            if (sendCoinsPage->handleURI(uri.toString()))
                nValidUrisFound++;
        }

        // if valid URIs were found
        if (nValidUrisFound)
            gotoSendCoinsPage();
        else
            notificator->notify(Notificator::Warning, tr("URI handling"), tr("URI can not be parsed! This can be caused by an invalid Innova address or malformed URI parameters."));
    }

    event->acceptProposedAction();
}

void BitcoinGUI::handleURI(QString strURI)
{
    // URI has to be valid
    if (sendCoinsPage->handleURI(strURI))
    {
        showNormalIfMinimized();
        gotoSendCoinsPage();
    }
    else
        notificator->notify(Notificator::Warning, tr("URI handling"), tr("URI can not be parsed! This can be caused by an invalid Innova address or malformed URI parameters."));
}



void BitcoinGUI::setEncryptionStatus(int status)
{
    switch(status)
    {
    case WalletModel::Unencrypted:
        labelEncryptionIcon->hide();
        encryptWalletAction->setChecked(false);
        changePassphraseAction->setEnabled(false);
        unlockWalletAction->setVisible(false);
        lockWalletAction->setVisible(false);
        encryptWalletAction->setEnabled(true);
        break;
    case WalletModel::Unlocked:
        disconnect(labelEncryptionIcon, SIGNAL(clicked()), unlockWalletAction, SLOT(trigger()));
        disconnect(labelEncryptionIcon, SIGNAL(clicked()),   lockWalletAction, SLOT(trigger()));
        connect   (labelEncryptionIcon, SIGNAL(clicked()),   lockWalletAction, SLOT(trigger()));

		if (fWalletUnlockStakingOnly)
        {
			labelEncryptionIcon->show();
            labelEncryptionIcon->setPixmap(MakeStatusIcon(":/icons/lock_staking", statusBar()->palette().color(QPalette::Highlight)));
			labelEncryptionIcon->setToolTip(tr("Wallet is <b>encrypted</b> and currently <b>unlocked for staking only</b>"));
        } else
        {
			labelEncryptionIcon->show();
			labelEncryptionIcon->setPixmap(MakeStatusIcon(":/icons/lock_open", statusBar()->palette().color(QPalette::Highlight)));
			labelEncryptionIcon->setToolTip(tr("Wallet is <b>encrypted</b> and currently <b>unlocked</b>"));
        };

        encryptWalletAction->setChecked(true);
        changePassphraseAction->setEnabled(true);
        unlockWalletAction->setVisible(false);
        lockWalletAction->setVisible(true);
        encryptWalletAction->setEnabled(false); 
        break;
    case WalletModel::Locked:
        disconnect(labelEncryptionIcon, SIGNAL(clicked()), unlockWalletAction, SLOT(trigger()));
        disconnect(labelEncryptionIcon, SIGNAL(clicked()),   lockWalletAction, SLOT(trigger()));
        connect   (labelEncryptionIcon, SIGNAL(clicked()), unlockWalletAction, SLOT(trigger()));
        labelEncryptionIcon->show();
        labelEncryptionIcon->setPixmap(MakeStatusIcon(":/icons/lock_closed", statusBar()->palette().color(QPalette::WindowText)));
        labelEncryptionIcon->setToolTip(tr("Wallet is <b>encrypted</b> and currently <b>locked</b>"));
        encryptWalletAction->setChecked(true);
        changePassphraseAction->setEnabled(true);
        unlockWalletAction->setVisible(true);
        lockWalletAction->setVisible(false);
        encryptWalletAction->setEnabled(false); 
        break;
    }
}

void BitcoinGUI::encryptWallet(bool status)
{
    if(!walletModel)
        return;
    AskPassphraseDialog dlg(status ? AskPassphraseDialog::Encrypt:
                                     AskPassphraseDialog::Decrypt, this);
    dlg.setModel(walletModel);
    dlg.exec();

    setEncryptionStatus(walletModel->getEncryptionStatus());
}

void BitcoinGUI::backupWallet()
{
    QString saveDir = QDesktopServices::storageLocation(QDesktopServices::DocumentsLocation);
    QString filename = QFileDialog::getSaveFileName(this, tr("Backup Wallet"), saveDir, tr("Wallet Data (*.dat)"));

    if(!filename.isEmpty()) {
        if(!walletModel->backupWallet(filename)) {
            QMessageBox::warning(this, tr("Backup Failed"), tr("There was an error trying to save your wallet data to %1.").arg(filename));
        } else {
            QMessageBox::warning(this, tr("Backup Successful!"), tr("Successfully saved your wallet data to %1.").arg(filename));
        }
    }
}

void BitcoinGUI::changePassphrase()
{
    AskPassphraseDialog dlg(AskPassphraseDialog::ChangePass, this);
    dlg.setModel(walletModel);
    dlg.exec();
}

void BitcoinGUI::unlockWallet()
{
    if(!walletModel)
        return;
    if(walletModel->getEncryptionStatus() == WalletModel::Locked)
    {
        AskPassphraseDialog::Mode mode = sender() == unlockWalletAction ?
        AskPassphraseDialog::UnlockStaking : AskPassphraseDialog::Unlock;
        AskPassphraseDialog dlg(mode, this);
        dlg.setModel(walletModel);
        dlg.exec();
    }
}

void BitcoinGUI::lockWallet()
{
    if(!walletModel)
        return;

    walletModel->setWalletLocked(true);
}

void BitcoinGUI::showNormalIfMinimized(bool fToggleHidden)
{
    // activateWindow() (sometimes) helps with keyboard focus on Windows
    if (isHidden())
    {
        show();
        activateWindow();
    }
    else if (isMinimized())
    {
        showNormal();
        activateWindow();
    }
    else if (GUIUtil::isObscured(this))
    {
        raise();
        activateWindow();
    }
    else if(fToggleHidden)
        hide();
}

void BitcoinGUI::toggleHidden()
{
    showNormalIfMinimized(true);
}

void BitcoinGUI::updateWeight()
{
    if (!pwalletMain)
        return;

    TRY_LOCK(cs_main, lockMain);
    if (!lockMain)
        return;

    TRY_LOCK(pwalletMain->cs_wallet, lockWallet);
    if (!lockWallet)
        return;

    uint64_t nMinWeight = 0, nMaxWeight = 0;
    pwalletMain->GetStakeWeight(*pwalletMain, nMinWeight, nMaxWeight, nWeight);
}

void BitcoinGUI::updateStakingIcon()
{
    updateWeight();

    if (!pwalletMain)
        return;

    bool fHasPeers;
    {
        TRY_LOCK(cs_vNodes, lockNodes);
        if (!lockNodes)
            return; // Skip this update if lock is contended
        fHasPeers = !vNodes.empty();
    }
    if (nWeight && GetBoolArg("-staking", true) && pwalletMain && !pwalletMain->IsLocked() && fHasPeers && !IsInitialBlockDownload())
    {
        uint64_t nNetworkWeight = GetPoSKernelPS();
        unsigned nEstimateTime = (10 * nTargetSpacing) * nNetworkWeight / nWeight;

        QString text;
        if (nEstimateTime < 60)
        {
            text = tr("%n second(s)", "", nEstimateTime);
        }
        else if (nEstimateTime < 60*60)
        {
            text = tr("%n minute(s)", "", nEstimateTime/60);
        }
        else if (nEstimateTime < 24*60*60)
        {
            text = tr("%n hour(s)", "", nEstimateTime/(60*60));
        }
        else
        {
            text = tr("%n day(s)", "", nEstimateTime/(60*60*24));
        }

        labelStakingIcon->setPixmap(MakeStatusIcon(":/icons/staking_on", statusBar()->palette().color(QPalette::Highlight)));
        labelStakingIcon->setToolTip(tr("Staking.<br>Your weight is %1<br>Network weight is %2<br>Expected time to earn reward is %3").arg(nWeight).arg(nNetworkWeight).arg(text));
    }
    else
    {
        labelStakingIcon->setPixmap(MakeStatusIcon(":/icons/staking_off", statusBar()->palette().color(QPalette::WindowText)));
        if (pwalletMain && pwalletMain->IsLocked())
            labelStakingIcon->setToolTip(tr("Not staking because wallet is locked"));
        else if (!fHasPeers)
            labelStakingIcon->setToolTip(tr("Not staking because wallet is offline"));
        else if (IsInitialBlockDownload())
            labelStakingIcon->setToolTip(tr("Not staking because wallet is syncing"));
        else if (!nWeight)
            labelStakingIcon->setToolTip(tr("Not staking because you don't have mature coins<br>Coins take 10 hours to mature."));
        else if (!GetBoolArg("-staking", true))
            labelStakingIcon->setToolTip(tr("Not staking because staking is disabled"));
        else
            labelStakingIcon->setToolTip(tr("Not staking"));
    }
}

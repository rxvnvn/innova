#include "stakingpage.h"
#include "walletmodel.h"
#include "optionsmodel.h"
#include "bitcoinunits.h"
#include "base58.h"
#include "main.h"
#include "wallet.h"
#include "init.h"

#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QTimer>
#include <QMessageBox>
#include <QFont>
#include <QScrollArea>
#include <QTabWidget>
#include <QFrame>
#include <QGridLayout>

StakingPage::StakingPage(QWidget *parent) :
    QWidget(parent),
    model(0),
    updateTimer(0)
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Status section (always visible)
    QGroupBox *statusGroup = new QGroupBox(tr("Staking Status"));
    QVBoxLayout *statusLayout = new QVBoxLayout(statusGroup);

    labelStakingMode = new QLabel(tr("Mode: Transparent"));
    QFont boldFont = labelStakingMode->font();
    boldFont.setBold(true);
    labelStakingMode->setFont(boldFont);
    statusLayout->addWidget(labelStakingMode);

    labelStakingStatus = new QLabel(tr("Status: Checking..."));
    statusLayout->addWidget(labelStakingStatus);

    labelStakingBalance = new QLabel(tr("Staking Balance: 0.00 INN"));
    statusLayout->addWidget(labelStakingBalance);

    labelEstimatedTime = new QLabel(tr("Estimated Time to Stake: N/A"));
    statusLayout->addWidget(labelEstimatedTime);

    mainLayout->addWidget(statusGroup);

    // Tab-based staking modes (replaces dropdown + stacked widget)
    stakingTabs = new QTabWidget();
    stakingTabs->setDocumentMode(true);

    setupTransparentPanel();
    setupColdStakingPanel();

    stakingTabs->addTab(transparentPanel, tr("Transparent"));
    stakingTabs->addTab(coldStakingPanel, tr("Cold Staking"));

    mainLayout->addWidget(stakingTabs);

    connect(stakingTabs, SIGNAL(currentChanged(int)), this, SLOT(onStakingModeChanged(int)));

    updateTimer = new QTimer(this);
    connect(updateTimer, SIGNAL(timeout()), this, SLOT(updateStakingStatus()));
    updateTimer->start(10000); // 10 second refresh
}

StakingPage::~StakingPage()
{
    if (updateTimer)
    {
        updateTimer->stop();
        updateTimer = 0;
    }
}

void StakingPage::setupTransparentPanel()
{
    transparentPanel = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(transparentPanel);

    QGroupBox *group = new QGroupBox(tr("Transparent Staking"));
    QVBoxLayout *groupLayout = new QVBoxLayout(group);

    QLabel *infoLabel = new QLabel(tr(
        "Transparent staking uses your regular coins to stake. "
        "This is the standard Proof-of-Stake mechanism. Your coins remain visible "
        "on the blockchain while staking."));
    infoLabel->setWordWrap(true);
    groupLayout->addWidget(infoLabel);

    labelTransparentUTXOs = new QLabel(tr("Stakeable UTXOs: Checking..."));
    groupLayout->addWidget(labelTransparentUTXOs);

    labelTransparentWeight = new QLabel(tr("Staking Weight: Checking..."));
    groupLayout->addWidget(labelTransparentWeight);

    layout->addWidget(group);
    layout->addStretch();
}


void StakingPage::setupColdStakingPanel()
{
    coldStakingPanel = new QWidget();
    QVBoxLayout *outerLayout = new QVBoxLayout(coldStakingPanel);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    QScrollArea *scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    QWidget *scrollContent = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(scrollContent);

    QGroupBox *infoGroup = new QGroupBox(tr("What is Cold Staking?"));
    QVBoxLayout *infoLayout = new QVBoxLayout(infoGroup);

    labelColdStakingInfo = new QLabel(tr(
        "Cold staking lets you earn staking rewards while keeping your spending keys "
        "completely offline. Your coins stay in your control - only the ability to stake "
        "is delegated to a hot node (typically a VPS).\n\n"
        "How it works:\n"
        "1. Create a staking address on your VPS node\n"
        "2. Delegate coins to that address from this wallet\n"
        "3. The VPS node stakes on your behalf 24/7\n"
        "4. Rewards go to your wallet, not the VPS\n"
        "5. You can revoke the delegation anytime\n\n"
        "Your Qt wallet can stay completely offline after delegation.\n"
        "The VPS staker node CANNOT spend your coins."));
    labelColdStakingInfo->setWordWrap(true);
    infoLayout->addWidget(labelColdStakingInfo);

    layout->addWidget(infoGroup);

    QGroupBox *setupGroup = new QGroupBox(tr("VPS Setup Instructions"));
    QVBoxLayout *setupLayout = new QVBoxLayout(setupGroup);

    labelColdInstructions = new QLabel(tr(
        "On your VPS, run these commands:\n\n"
        "  innovad -staking=1\n"
        "  innova-cli getnewstakingaddress\n\n"
        "Copy the staking address from your VPS and paste it below."));
    labelColdInstructions->setWordWrap(true);
    labelColdInstructions->setTextInteractionFlags(Qt::TextSelectableByMouse);
    setupLayout->addWidget(labelColdInstructions);

    layout->addWidget(setupGroup);

    QGroupBox *delegateGroup = new QGroupBox(tr("Delegate Coins"));
    QVBoxLayout *delegateLayout = new QVBoxLayout(delegateGroup);

    QHBoxLayout *addrRow = new QHBoxLayout();
    addrRow->addWidget(new QLabel(tr("VPS Staking Address:")));
    editStakerAddress = new QLineEdit();
    editStakerAddress->setPlaceholderText(tr("Paste staking address from your VPS"));
    addrRow->addWidget(editStakerAddress);
    delegateLayout->addLayout(addrRow);

    QHBoxLayout *amountRow = new QHBoxLayout();
    amountRow->addWidget(new QLabel(tr("Amount to Delegate:")));
    editDelegateAmount = new QLineEdit();
    editDelegateAmount->setPlaceholderText(tr("Amount in INN"));
    amountRow->addWidget(editDelegateAmount);
    delegateLayout->addLayout(amountRow);

    btnDelegate = new QPushButton(tr("Delegate Stake"));
    btnDelegate->setToolTip(tr("Delegate the specified amount to the VPS staking address"));
    btnDelegate->setMaximumWidth(200);
    connect(btnDelegate, SIGNAL(clicked()), this, SLOT(onDelegateClicked()));
    delegateLayout->addWidget(btnDelegate);

    layout->addWidget(delegateGroup);

    QGroupBox *activeGroup = new QGroupBox(tr("Active Delegations"));
    QVBoxLayout *activeLayout = new QVBoxLayout(activeGroup);

    labelColdBalance = new QLabel(tr("Total Delegated: 0.00 INN"));
    QFont boldFont = labelColdBalance->font();
    boldFont.setBold(true);
    labelColdBalance->setFont(boldFont);
    activeLayout->addWidget(labelColdBalance);

    tableDelegations = new QTableWidget(0, 4);
    tableDelegations->setHorizontalHeaderLabels(
        QStringList() << tr("Amount") << tr("Staker Address") << tr("Owner Address") << tr("Confirmations"));
    tableDelegations->horizontalHeader()->setStretchLastSection(true);
    tableDelegations->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableDelegations->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableDelegations->setMinimumHeight(150);
    activeLayout->addWidget(tableDelegations);

    QHBoxLayout *btnRow = new QHBoxLayout();
    btnRefreshDelegations = new QPushButton(tr("Refresh"));
    connect(btnRefreshDelegations, SIGNAL(clicked()), this, SLOT(onRefreshDelegations()));
    btnRow->addWidget(btnRefreshDelegations);
    btnRow->addStretch();
    activeLayout->addLayout(btnRow);

    layout->addWidget(activeGroup);

    scrollArea->setWidget(scrollContent);
    outerLayout->addWidget(scrollArea);
}


void StakingPage::setModel(WalletModel *model)
{
    this->model = model;
    if (model)
    {
        {
            LOCK(cs_stakingMode);
            stakingTabs->setCurrentIndex((nStakingMode == STAKE_COLD) ? 1 : 0);
        }

        if (model->getOptionsModel())
        {
            connect(model->getOptionsModel(), SIGNAL(stakingModeChanged(int)),
                    stakingTabs, SLOT(setCurrentIndex(int)));
        }

        updateStakingStatus();
        updateBalances();
    }
}

void StakingPage::onStakingModeChanged(int index)
{
    if (index < 0 || index > 1)
        return;

    // Map tab index to staking mode: 0=transparent (STAKE_TRANSPARENT), 1=cold (STAKE_COLD)
    int modeIdx = (index == 1) ? 2 : 0;
    {
        LOCK(cs_stakingMode);
        nStakingMode = (StakingMode)modeIdx;
    }

    updateModeDescription(index);

    if (model && model->getOptionsModel())
    {
        model->getOptionsModel()->setData(
            model->getOptionsModel()->index(OptionsModel::StakingModeOpt),
            QVariant(modeIdx), Qt::EditRole);
    }

    updateBalances();
}

void StakingPage::updateModeDescription(int mode)
{
    switch(mode)
    {
    case 0:
        labelStakingMode->setText(tr("Mode: Transparent Staking"));
        break;
    case 1:
        labelStakingMode->setText(tr("Mode: Cold Staking (Delegated)"));
        break;
    default:
        labelStakingMode->setText(tr("Mode: Unknown"));
        break;
    }
}

void StakingPage::updateStakingStatus()
{
    if (!model)
        return;

    bool fStaking = GetBoolArg("-staking", true);
    bool fLocked = pwalletMain ? pwalletMain->IsLocked() : true;
    bool fConnected;
    {
        TRY_LOCK(cs_vNodes, lockNodes);
        if (!lockNodes)
            return; // Skip this update if lock is contended
        fConnected = !vNodes.empty();
    }
    bool fSyncing = IsInitialBlockDownload();

    if (!fStaking)
        labelStakingStatus->setText(tr("Status: Staking is disabled (-staking=0)"));
    else if (fLocked)
        labelStakingStatus->setText(tr("Status: Wallet is locked. Unlock for staking."));
    else if (!fConnected)
        labelStakingStatus->setText(tr("Status: Not connected to any peers."));
    else if (fSyncing)
        labelStakingStatus->setText(tr("Status: Synchronizing blockchain..."));
    else
        labelStakingStatus->setText(tr("Status: Staking active"));
}

void StakingPage::updateBalances()
{
    if (!model || !pwalletMain)
        return;

    TRY_LOCK(pwalletMain->cs_wallet, lockWallet);
    if (!lockWallet)
        return;

    int mode = stakingTabs->currentIndex();
    int unit = BitcoinUnits::BTC;
    if (model->getOptionsModel())
        unit = model->getOptionsModel()->getDisplayUnit();

    switch(mode)
    {
    case 0: // Transparent
    {
        int64_t nBalance = pwalletMain->GetBalance();
        labelStakingBalance->setText(tr("Staking Balance: %1")
            .arg(BitcoinUnits::formatWithUnit(unit, nBalance)));
        break;
    }
    case 1: // Cold
    {
        int64_t nCold = pwalletMain->GetColdStakingBalance();
        labelStakingBalance->setText(tr("Cold Staking Balance: %1")
            .arg(BitcoinUnits::formatWithUnit(unit, nCold)));
        labelColdBalance->setText(tr("Total Delegated: %1")
            .arg(BitcoinUnits::formatWithUnit(unit, nCold)));
        break;
    }
    default:
        break;
    }
}

void StakingPage::onDelegateClicked()
{
    if (!model || !pwalletMain)
        return;

    QString stakerAddr = editStakerAddress->text().trimmed();
    QString amountStr = editDelegateAmount->text().trimmed();

    if (stakerAddr.isEmpty())
    {
        QMessageBox::warning(this, tr("Delegation Error"),
            tr("Please enter the VPS staking address."));
        return;
    }

    if (amountStr.isEmpty())
    {
        QMessageBox::warning(this, tr("Delegation Error"),
            tr("Please enter the amount to delegate."));
        return;
    }

    bool ok;
    double amountDbl = amountStr.toDouble(&ok);
    if (!ok || amountDbl <= 0 || amountDbl > 18000000.0) // MAX_MONEY is ~18M INN
    {
        QMessageBox::warning(this, tr("Delegation Error"),
            tr("Invalid amount. Please enter a positive number up to 18,000,000 INN."));
        return;
    }
    int64_t nAmount = (int64_t)(amountDbl * COIN + 0.5);
    if (nAmount <= 0 || nAmount > MAX_MONEY)
    {
        QMessageBox::warning(this, tr("Delegation Error"),
            tr("Amount out of valid range."));
        return;
    }

    CBitcoinAddress addrCheck(stakerAddr.toStdString());
    if (!addrCheck.IsValid())
    {
        QMessageBox::warning(this, tr("Delegation Error"),
            tr("Invalid staking address. Please check the address and try again."));
        return;
    }

    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(this, tr("Confirm Delegation"),
        tr("Delegate %1 INN to staker address:\n%2\n\n"
           "The VPS will stake these coins on your behalf. "
           "You can revoke this delegation at any time.\n\nProceed?")
           .arg(amountStr, stakerAddr),
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes)
    {
        QMessageBox::information(this, tr("Delegate Stake"),
            tr("To delegate your stake, use the Debug Console (Help > Debug Window > Console):\n\n"
               "  delegatestake \"%1\" %2\n\n"
               "This will create a cold staking delegation transaction.\n"
               "This will be integrated directly in a future update.")
               .arg(stakerAddr, amountStr));
    }
}

void StakingPage::onRevokeDelegation()
{
    QMessageBox::information(this, tr("Revoke Delegation"),
        tr("To revoke a cold staking delegation, use the Debug Console:\n\n"
           "  revokecoldstaking <txid> <vout>\n\n"
           "Use 'listcoldutxos' to see your active delegations."));
}

void StakingPage::onRefreshDelegations()
{
    if (!model || !pwalletMain)
        return;

    tableDelegations->setRowCount(0);

    int64_t nCold = pwalletMain->GetColdStakingBalance();
    int unit = BitcoinUnits::BTC;
    if (model->getOptionsModel())
        unit = model->getOptionsModel()->getDisplayUnit();

    labelColdBalance->setText(tr("Total Delegated: %1")
        .arg(BitcoinUnits::formatWithUnit(unit, nCold)));

    if (nCold == 0)
    {
        QMessageBox::information(this, tr("Cold Staking"),
            tr("No active cold staking delegations found.\n\n"
               "To create a delegation, enter a VPS staking address and amount above."));
    }
}

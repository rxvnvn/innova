#include "privacypage.h"
#include "walletmodel.h"
#include "bitcoinunits.h"
#include "optionsmodel.h"
#include "guiconstants.h"

#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QListWidget>
#include <QStringList>

PrivacyPage::PrivacyPage(QWidget *parent) :
    QWidget(parent),
    model(0)
{
    setupUI();
}

void PrivacyPage::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Title
    QLabel *titleLabel = new QLabel(tr("Silent Payment"));
    titleLabel->setStyleSheet("font-size: 18px; font-weight: bold; margin-bottom: 10px;");
    mainLayout->addWidget(titleLabel);

    QLabel *descLabel = new QLabel(tr("Silent Payment addresses provide stealth receiving. Each sender derives a unique "
                                      "one-time address from your SP address, so no two payments share an on-chain address. "
                                      "Share your SP address publicly — receivers cannot be linked on the blockchain."));
    descLabel->setWordWrap(true);
    descLabel->setStyleSheet("color: #888; margin-bottom: 15px;");
    mainLayout->addWidget(descLabel);

    spAddressList = new QListWidget();
    spAddressList->setStyleSheet("QListWidget { font-family: monospace; font-size: 11px; }");
    mainLayout->addWidget(spAddressList);

    QHBoxLayout *spButtons = new QHBoxLayout();
    newSPAddressButton = new QPushButton(tr("New SP Address"));
    newSPAddressButton->setStyleSheet("QPushButton { background-color: #9C27B0; color: white; padding: 6px 12px; font-weight: bold; }");
    copySPAddressButton = new QPushButton(tr("Copy Selected"));
    spButtons->addWidget(newSPAddressButton);
    spButtons->addWidget(copySPAddressButton);
    spButtons->addStretch();
    mainLayout->addLayout(spButtons);

    // Status bar
    statusLabel = new QLabel(tr("Ready"));
    statusLabel->setStyleSheet("color: #888; margin-top: 10px;");
    mainLayout->addWidget(statusLabel);

    // Connect signals
    connect(newSPAddressButton, SIGNAL(clicked()), this, SLOT(onNewSPAddressClicked()));
    connect(copySPAddressButton, SIGNAL(clicked()), this, SLOT(onCopySPAddressClicked()));
}

void PrivacyPage::setModel(WalletModel *model)
{
    this->model = model;
    if (model)
        refreshSPAddresses();
}

void PrivacyPage::refreshSPAddresses()
{
    if (!model)
        return;

    spAddressList->clear();
    QStringList addrs = model->getSilentPaymentAddresses();
    for (const QString& addr : addrs)
        spAddressList->addItem(addr);

    if (addrs.isEmpty())
        spAddressList->addItem(tr("No silent payment addresses yet. Click 'New SP Address' to create one."));
}

void PrivacyPage::onNewSPAddressClicked()
{
    if (!model)
        return;

    WalletModel::UnlockContext ctx(model->requestUnlock());
    if (!ctx.isValid())
        return;

    QString newAddr = model->getNewSilentPaymentAddress();
    if (newAddr.isEmpty())
    {
        QMessageBox::warning(this, tr("Error"), tr("Failed to generate silent payment address. Check that your wallet is unlocked."));
        return;
    }

    statusLabel->setText(tr("New SP address created"));
    refreshSPAddresses();

    // Copy to clipboard
    QApplication::clipboard()->setText(newAddr);
    QMessageBox::information(this, tr("New Silent Payment Address"),
        tr("Your new silent payment address has been created and copied to clipboard:\n\n%1\n\n"
           "Share this address publicly. Each sender will derive a unique one-time address, "
           "so payments cannot be linked on the blockchain.").arg(newAddr));
}

void PrivacyPage::onCopySPAddressClicked()
{
    QListWidgetItem *item = spAddressList->currentItem();
    if (item && !item->text().startsWith("No silent"))
    {
        QApplication::clipboard()->setText(item->text());
        statusLabel->setText(tr("SP address copied to clipboard"));
    }
}

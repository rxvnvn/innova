#include "overviewpage.h"
#include "ui_overviewpage.h"

#include "walletmodel.h"
#include "bitcoinunits.h"
#include "optionsmodel.h"
#include "transactiontablemodel.h"
#include "transactionfilterproxy.h"
#include "guiutil.h"
#include "guiconstants.h"
#include "main.h"

#include <QAbstractItemDelegate>
#include <QFrame>
#include <QFont>
#include <QFontMetrics>
#include <QListView>
#include <QPainter>
#include <QPalette>
#include <QSizePolicy>

#define DECORATION_SIZE 40
#define NUM_ITEMS 5


class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    TxViewDelegate(): QAbstractItemDelegate(), unit(BitcoinUnits::BTC)
    {

    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        QRect mainRect = option.rect.adjusted(4, 4, -4, -4);
        QRect decorationRect(mainRect.left(), mainRect.top() + (mainRect.height() - DECORATION_SIZE) / 2, DECORATION_SIZE, DECORATION_SIZE);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString dateText = GUIUtil::dateTimeStr(date);
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = option.palette.color(QPalette::Text);
        if(qVariantCanConvert<QColor>(value))
        {
            foreground = qvariant_cast<QColor>(value);
        }

        QString amountText = BitcoinUnits::formatWithUnit(unit, amount, true);
        if(!confirmed)
        {
            amountText = QString("[") + amountText + QString("]");
        }

        QFont amountFont = option.font;
        amountFont.setBold(true);
        QFontMetrics amountMetrics(amountFont);
        QFontMetrics textMetrics(option.font);
        int amountWidth = amountMetrics.width(amountText) + 12;
        if(amountWidth < 110)
            amountWidth = 110;

        QRect textRect = mainRect.adjusted(DECORATION_SIZE + 10, 0, 0, 0);
        int maxAmountWidth = textRect.width() / 2;
        if(maxAmountWidth > 0 && amountWidth > maxAmountWidth)
            amountWidth = maxAmountWidth;

        QRect amountRect(textRect.right() - amountWidth + 1, textRect.top(), amountWidth, textRect.height());
        QRect detailRect = textRect;
        detailRect.setRight(amountRect.left() - 8);
        int halfheight = detailRect.height() / 2;
        QRect dateRect(detailRect.left(), detailRect.top(), detailRect.width(), halfheight);
        QRect addressRect(detailRect.left(), detailRect.top() + halfheight, detailRect.width(), detailRect.height() - halfheight);

        painter->setPen(option.palette.color(QPalette::Mid));
        painter->drawText(dateRect, Qt::AlignLeft|Qt::AlignVCenter, textMetrics.elidedText(dateText, Qt::ElideRight, dateRect.width()));

        painter->setPen(foreground);
        painter->drawText(addressRect, Qt::AlignLeft|Qt::AlignVCenter, textMetrics.elidedText(address, Qt::ElideRight, addressRect.width()));

        if(amount < 0)
        {
            foreground = COLOR_NEGATIVE;
        }
        else if(!confirmed)
        {
            foreground = COLOR_UNCONFIRMED;
        }
        else
        {
            foreground = option.palette.color(QPalette::Text);
        }
        painter->setPen(foreground);
        painter->setFont(amountFont);
        painter->drawText(amountRect, Qt::AlignRight|Qt::AlignVCenter, amountMetrics.elidedText(amountText, Qt::ElideLeft, amountRect.width()));

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        return QSize(DECORATION_SIZE + 220, DECORATION_SIZE + 8);
    }

    int unit;

};
#include "overviewpage.moc"

OverviewPage::OverviewPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OverviewPage),
    model(0),
    currentBalance(-1),
    currentLockedBalance(-1),
    currentUnconfirmedBalance(-1),
    currentImmatureBalance(-1),
    currentWatchOnlyBalance(-1),
    currentWatchUnconfBalance(-1),
    currentWatchImmatureBalance(-1),
    currentShieldedBalance(-1),
    totalBalance(-1),
    txdelegate(new TxViewDelegate()),
    filter(0)
{
    ui->setupUi(this);

    setWindowOpacity(1.0);
    setStyleSheet(QString());

    ui->label_5->setText(tr("Balances"));
    ui->label_4->setText(tr("Recent transactions"));
    ui->label->setText(tr("Available:"));
    ui->labelTotalText->setText(tr("Total:"));

    ui->frame->setFrameShape(QFrame::NoFrame);
    ui->frame->setFrameShadow(QFrame::Plain);
    ui->frame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    ui->frame_2->setFrameShape(QFrame::NoFrame);
    ui->frame_2->setFrameShadow(QFrame::Plain);
    ui->frame_2->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    ui->line_2->hide();
    ui->line_3->hide();

    ui->gridLayout_3->setContentsMargins(14, 14, 14, 14);
    ui->gridLayout_3->setHorizontalSpacing(18);
    ui->gridLayout_2->setContentsMargins(0, 0, 0, 0);
    ui->gridLayout_2->setVerticalSpacing(8);
    ui->gridLayout_4->setContentsMargins(0, 0, 0, 0);
    ui->gridLayout_4->setVerticalSpacing(8);
    ui->gridLayout->setHorizontalSpacing(18);
    ui->gridLayout->setVerticalSpacing(5);
    ui->gridLayout->setColumnMinimumWidth(0, 150);
    ui->frame->setMinimumWidth(0);
    ui->frame_2->setMinimumWidth(0);
    ui->gridLayout_3->setColumnStretch(0, 1);
    ui->gridLayout_3->setColumnStretch(1, 1);

    QFont headingFont = font();
    headingFont.setWeight(QFont::DemiBold);
    ui->label_5->setFont(headingFont);
    ui->label_4->setFont(headingFont);

    QFont balanceFont = font();
    ui->labelBalance->setFont(balanceFont);
    ui->labelTotal->setFont(balanceFont);
    ui->labelLocked->setFont(balanceFont);
    ui->labelStake->setFont(balanceFont);
    ui->labelShielded->setFont(balanceFont);
    ui->labelUnconfirmed->setFont(balanceFont);
    ui->labelImmature->setFont(balanceFont);
    ui->labelWatchAvailable->setFont(balanceFont);
    ui->labelWatchPending->setFont(balanceFont);
    ui->labelWatchImmature->setFont(balanceFont);
    ui->labelWatchTotal->setFont(balanceFont);

    QFont warningFont = font();
    ui->labelWalletStatus->setFont(warningFont);
    ui->labelTransactionsStatus->setFont(warningFont);
    ui->labelWalletStatus->setContentsMargins(6, 0, 0, 0);
    ui->labelTransactionsStatus->setContentsMargins(6, 0, 0, 0);
    ui->labelWalletStatus->setStyleSheet(QString());
    ui->labelTransactionsStatus->setStyleSheet(QString());

    ui->labelBalance->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->labelLocked->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->labelStake->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->labelShielded->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->labelUnconfirmed->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->labelImmature->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->labelTotal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->labelWatchAvailable->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->labelWatchPending->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->labelWatchImmature->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->labelWatchTotal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->listTransactions->setFrameShape(QFrame::NoFrame);
    ui->listTransactions->setSpacing(2);
    ui->listTransactions->setUniformItemSizes(true);

    connect(ui->listTransactions, SIGNAL(clicked(QModelIndex)), this, SLOT(handleTransactionClicked(QModelIndex)));

    // init "out of sync" warning labels
    ui->labelWalletStatus->setText("(" + tr("Out of Sync!") + ")");
    ui->labelTransactionsStatus->setText("(" + tr("Out of Sync!") + ")");

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        emit transactionClicked(filter->mapToSource(index));
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::setBalance(qint64 balance, qint64 lockedbalance, qint64 stake, qint64 unconfirmedBalance, qint64 immatureBalance, qint64 watchOnlyBalance, qint64 watchUnconfBalance, qint64 watchImmatureBalance, qint64 shieldedBalance)
{
    if (!model || !model->getOptionsModel())
        return;
    int unit = model->getOptionsModel()->getDisplayUnit();
    currentBalance = balance;
    currentLockedBalance = lockedbalance;
    currentUnconfirmedBalance = unconfirmedBalance;
    currentImmatureBalance = immatureBalance;
    currentShieldedBalance = shieldedBalance;

    currentWatchOnlyBalance = watchOnlyBalance;
    currentWatchUnconfBalance = watchUnconfBalance;
    currentWatchImmatureBalance = watchImmatureBalance;
    totalBalance = balance + lockedbalance + unconfirmedBalance + immatureBalance;

    ui->labelBalance->setText(BitcoinUnits::formatWithUnit(unit, balance));
    ui->labelLocked->setText(BitcoinUnits::formatWithUnit(unit, lockedbalance));

    ui->labelStake->setText(BitcoinUnits::formatWithUnit(unit, stake));
    ui->labelStake->setToolTip(tr("Stake balance"));

    // Shielded (privacy) balance is always visible so users know it exists.
    if (ui->labelShielded)
    {
        ui->labelShielded->setText(BitcoinUnits::formatWithUnit(unit, shieldedBalance));
        ui->labelShielded->setToolTip(tr("Shielded (private) balance. Shield coins via the Send page to move funds here."));
    }

    // Include shielded in total
    totalBalance += shieldedBalance;

    ui->labelUnconfirmed->setText(BitcoinUnits::formatWithUnit(unit, unconfirmedBalance));
    ui->labelImmature->setText(BitcoinUnits::formatWithUnit(unit, immatureBalance));
    ui->labelTotal->setText(BitcoinUnits::formatWithUnit(unit, totalBalance));

    //Watch Only Balances
    ui->labelWatchAvailable->setText(BitcoinUnits::formatWithUnit(unit, watchOnlyBalance));
    ui->labelWatchPending->setText(BitcoinUnits::formatWithUnit(unit, watchUnconfBalance));
    ui->labelWatchImmature->setText(BitcoinUnits::formatWithUnit(unit, watchImmatureBalance));
    ui->labelWatchTotal->setText(BitcoinUnits::formatWithUnit(unit, watchOnlyBalance + watchUnconfBalance + watchImmatureBalance));


    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature = immatureBalance != 0;
    bool showLocked = lockedbalance != 0;
    bool showWatchImmature = watchImmatureBalance != 0;
    bool showStakeBalance = GetBoolArg("-staking", true);

    ui->labelImmature->setVisible(showImmature);
    ui->labelImmatureText->setVisible(showImmature);
    ui->labelWatchImmature->setVisible(showWatchImmature);
    ui->labelWatchImmatureText->setVisible(showWatchImmature);
    ui->labelLocked->setVisible(showLocked);
    ui->labelLockedText->setVisible(showLocked);
    ui->labelStake->setVisible(showStakeBalance);
    ui->labelStakeText->setVisible(showStakeBalance);

}

void OverviewPage::updateWatchOnlyLabels(bool showWatchOnly)
{
    //ui->labelSpendable->setVisible(showWatchOnly);      // show spendable label (only when watch-only is active)
    ui->labelWatchonly->setVisible(showWatchOnly);      // show watch-only label
    //ui->lineWatchBalance->setVisible(showWatchOnly);    // show watch-only balance separator line
    ui->labelWatchAvailable->setVisible(showWatchOnly); // show watch-only available balance
    ui->labelWatchPending->setVisible(showWatchOnly);   // show watch-only pending balance
    ui->labelWatchTotal->setVisible(showWatchOnly);     // show watch-only total balance

	ui->watch1->setVisible(showWatchOnly);
	ui->watch2->setVisible(showWatchOnly);
    ui->labelWatchImmatureText->setVisible(showWatchOnly);
	ui->watch4->setVisible(showWatchOnly);

    if (!showWatchOnly)
        ui->labelWatchImmature->hide();
 }

void OverviewPage::setModel(WalletModel *model)
{
    this->model = model;
    if(model && model->getOptionsModel())
    {
        // Set up transaction list
        filter = new TransactionFilterProxy();
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setLimit(NUM_ITEMS);
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter);
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        // Keep up to date with wallet
        setBalance(model->getUnlockedBalance(), model->getLockedBalance(), model->getStakeAmount(), model->getUnconfirmedBalance(), model->getImmatureBalance(), model->getWatchBalance(), model->getWatchUnconfirmedBalance(), model->getWatchImmatureBalance(), model->getShieldedBalance());
        connect(model, SIGNAL(balanceChanged(qint64, qint64, qint64, qint64, qint64, qint64, qint64, qint64, qint64)), this, SLOT(setBalance(qint64, qint64, qint64, qint64, qint64, qint64, qint64, qint64, qint64)));

        // Watch Only
        updateWatchOnlyLabels(model->haveWatchOnly());
        connect(model, SIGNAL(notifyWatchonlyChanged(bool)), this, SLOT(updateWatchOnlyLabels(bool)));

        connect(model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));
    }

    // update the display unit, to not use the default ("BTC")
    updateDisplayUnit();
}

void OverviewPage::updateDisplayUnit()
{
    if(model && model->getOptionsModel())
    {
        if(currentBalance != -1)
            setBalance(currentBalance, currentLockedBalance, model->getStakeAmount(), currentUnconfirmedBalance, currentImmatureBalance, currentWatchOnlyBalance, currentWatchUnconfBalance, currentWatchImmatureBalance, currentShieldedBalance);

        // Update txdelegate->unit with the current unit
        txdelegate->unit = model->getOptionsModel()->getDisplayUnit();

        ui->listTransactions->update();
    }
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}

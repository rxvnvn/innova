#include "transactionview.h"

#include "transactionfilterproxy.h"
#include "transactionrecord.h"
#include "walletmodel.h"
#include "addresstablemodel.h"
#include "transactiontablemodel.h"
#include "bitcoinunits.h"
#include "csvmodelwriter.h"
#include "transactiondescdialog.h"
#include "editaddressdialog.h"
#include "optionsmodel.h"
#include "guiutil.h"

#include <QComboBox>
#include <QDateTimeEdit>
#include <QDoubleValidator>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QAbstractItemView>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPoint>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTableView>
#include <QVBoxLayout>

TransactionView::TransactionView(QWidget *parent) :
    QWidget(parent), model(0), transactionProxyModel(0),
    transactionView(0), transactionSum(0), pageTitle(0),
    searchLabel(0), dateLabel(0), typeLabel(0), amountLabel(0),
    exportButton(0), contentStack(0), tablePage(0), emptyPage(0),
    emptyStateTitle(0), emptyStateText(0), dateWidget(0), typeWidget(0),
    addressWidget(0), amountWidget(0), contextMenu(0), dateRangeWidget(0),
    dateFrom(0), dateTo(0)
{
    // Build filter row.
    setContentsMargins(0, 0, 0, 0);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(8);

    QString pageTitleText = tr("&Transactions");
    pageTitleText.remove('&');
    pageTitle = new QLabel(pageTitleText, this);
    QFont titleFont = pageTitle->font();
    titleFont.setBold(true);
    if (titleFont.pointSize() > 0) {
        titleFont.setPointSize(titleFont.pointSize() + 1);
    }
    pageTitle->setFont(titleFont);
    mainLayout->addWidget(pageTitle);

    QGridLayout *filterLayout = new QGridLayout();
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setHorizontalSpacing(8);
    filterLayout->setVerticalSpacing(4);

    searchLabel = new QLabel(tr("&Search:"), this);
    filterLayout->addWidget(searchLabel, 0, 0);

    addressWidget = new QLineEdit(this);
#if QT_VERSION >= 0x040700
    addressWidget->setPlaceholderText(tr("Enter address or label to search"));
#endif
    addressWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    searchLabel->setBuddy(addressWidget);
    filterLayout->addWidget(addressWidget, 0, 1, 1, 3);

    exportButton = new QPushButton(QIcon(":/icons/export"), tr("&Export..."), this);
    exportButton->setAutoDefault(false);
    exportButton->setToolTip(tr("Export the data in the current tab to a file"));
    filterLayout->addWidget(exportButton, 0, 4, Qt::AlignRight | Qt::AlignVCenter);

    dateLabel = new QLabel(tr("&Date:"), this);
    filterLayout->addWidget(dateLabel, 1, 0);

    dateWidget = new QComboBox(this);
#ifdef Q_OS_MAC
    dateWidget->setFixedWidth(121);
#else
    dateWidget->setFixedWidth(120);
#endif
    dateWidget->addItem(tr("All"), All);
    dateWidget->addItem(tr("Today"), Today);
    dateWidget->addItem(tr("This week"), ThisWeek);
    dateWidget->addItem(tr("This month"), ThisMonth);
    dateWidget->addItem(tr("Last month"), LastMonth);
    dateWidget->addItem(tr("This year"), ThisYear);
    dateWidget->addItem(tr("Range..."), Range);
    dateLabel->setBuddy(dateWidget);
    filterLayout->addWidget(dateWidget, 1, 1);

    typeLabel = new QLabel(tr("&Type:"), this);
    filterLayout->addWidget(typeLabel, 1, 2);

    typeWidget = new QComboBox(this);
#ifdef Q_OS_MAC
    typeWidget->setFixedWidth(121);
#else
    typeWidget->setFixedWidth(120);
#endif
    typeWidget->addItem(tr("All"), TransactionFilterProxy::ALL_TYPES);
    typeWidget->addItem(tr("Received with"), TransactionFilterProxy::TYPE(TransactionRecord::RecvWithAddress) |
                                        TransactionFilterProxy::TYPE(TransactionRecord::RecvFromOther));
    typeWidget->addItem(tr("Sent to"), TransactionFilterProxy::TYPE(TransactionRecord::SendToAddress) |
                                  TransactionFilterProxy::TYPE(TransactionRecord::SendToOther));
    typeWidget->addItem(tr("To yourself"), TransactionFilterProxy::TYPE(TransactionRecord::SendToSelf));
    typeWidget->addItem(tr("Mined"), TransactionFilterProxy::TYPE(TransactionRecord::Generated));
    typeWidget->addItem(tr("Other"), TransactionFilterProxy::TYPE(TransactionRecord::Other));
    typeLabel->setBuddy(typeWidget);
    filterLayout->addWidget(typeWidget, 1, 3);

    amountLabel = new QLabel(tr("Min &amount:"), this);
    filterLayout->addWidget(amountLabel, 1, 4);

    amountWidget = new QLineEdit(this);
#if QT_VERSION >= 0x040700
    amountWidget->setPlaceholderText(tr("Min amount"));
#endif
#ifdef Q_OS_MAC
    amountWidget->setFixedWidth(97);
#else
    amountWidget->setFixedWidth(100);
#endif
    amountWidget->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    amountWidget->setValidator(new QDoubleValidator(0, 1e20, 8, this));
    amountLabel->setBuddy(amountWidget);
    filterLayout->addWidget(amountWidget, 1, 5);

    filterLayout->setColumnStretch(1, 1);
    filterLayout->setColumnStretch(3, 1);
    filterLayout->setColumnStretch(5, 0);
    mainLayout->addLayout(filterLayout);

    mainLayout->addWidget(createDateRangeWidget());

    contentStack = new QStackedWidget(this);
    contentStack->setContentsMargins(0, 0, 0, 0);

    tablePage = new QWidget(contentStack);
    QVBoxLayout *tableLayout = new QVBoxLayout(tablePage);
    tableLayout->setContentsMargins(0, 0, 0, 0);
    tableLayout->setSpacing(0);

    QTableView *view = new QTableView(tablePage);
    view->setObjectName("transactionView");
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    view->setTabKeyNavigation(false);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    view->setAlternatingRowColors(true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setSortingEnabled(true);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setWordWrap(false);
    view->setShowGrid(false);
    view->verticalHeader()->hide();
    transactionView = view;
    tableLayout->addWidget(transactionView);

    emptyPage = new QWidget(contentStack);
    QVBoxLayout *emptyLayout = new QVBoxLayout(emptyPage);
    emptyLayout->setContentsMargins(0, 18, 0, 18);
    emptyLayout->setSpacing(6);
    emptyLayout->addStretch();

    emptyStateTitle = new QLabel(tr("No transactions yet."), emptyPage);
    QFont emptyTitleFont = emptyStateTitle->font();
    emptyTitleFont.setBold(true);
    if (emptyTitleFont.pointSize() > 0) {
        emptyTitleFont.setPointSize(emptyTitleFont.pointSize() + 1);
    }
    emptyStateTitle->setFont(emptyTitleFont);
    emptyStateTitle->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    emptyLayout->addWidget(emptyStateTitle);

    emptyStateText = new QLabel(tr("Transactions matching the current filters will appear here."), emptyPage);
    emptyStateText->setWordWrap(true);
    emptyStateText->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    emptyLayout->addWidget(emptyStateText);
    emptyLayout->addStretch();

    contentStack->addWidget(tablePage);
    contentStack->addWidget(emptyPage);
    mainLayout->addWidget(contentStack, 1);

    QFrame *summaryFrame = new QFrame(this);
    summaryFrame->setFrameShape(QFrame::NoFrame);
    QHBoxLayout *summaryLayout = new QHBoxLayout(summaryFrame);
    summaryLayout->setContentsMargins(0, 0, 0, 0);
    summaryLayout->setSpacing(6);

    QLabel* transactionSumLabel = new QLabel(tr("Selected amount:"), summaryFrame);
    transactionSumLabel->setObjectName("transactionSumLabel");
    summaryLayout->addWidget(transactionSumLabel);

    transactionSum = new QLabel(summaryFrame);
    transactionSum->setObjectName("transactionSum");
    transactionSum->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summaryLayout->addWidget(transactionSum);
    summaryLayout->addStretch();
    mainLayout->addWidget(summaryFrame);

    // Actions
    QAction *copyAddressAction = new QAction(tr("Copy address"), this);
    QAction *copyLabelAction = new QAction(tr("Copy label"), this);
    QAction *copyAmountAction = new QAction(tr("Copy amount"), this);
    QAction *copyTxIDAction = new QAction(tr("Copy transaction ID"), this);
    QAction *editLabelAction = new QAction(tr("Edit label"), this);
    QAction *showDetailsAction = new QAction(tr("Show transaction details"), this);

    contextMenu = new QMenu(this);
    contextMenu->addAction(copyAddressAction);
    contextMenu->addAction(copyLabelAction);
    contextMenu->addAction(copyAmountAction);
    contextMenu->addAction(copyTxIDAction);
    contextMenu->addSeparator();
    contextMenu->addAction(editLabelAction);
    contextMenu->addAction(showDetailsAction);

    // Connect actions
    connect(exportButton, SIGNAL(clicked()), this, SLOT(exportClicked()));
    connect(dateWidget, SIGNAL(activated(int)), this, SLOT(chooseDate(int)));
    connect(typeWidget, SIGNAL(activated(int)), this, SLOT(chooseType(int)));
    connect(addressWidget, SIGNAL(textChanged(QString)), this, SLOT(changedPrefix(QString)));
    connect(amountWidget, SIGNAL(textChanged(QString)), this, SLOT(changedAmount(QString)));

    connect(view, SIGNAL(doubleClicked(QModelIndex)), this, SIGNAL(doubleClicked(QModelIndex)));
    connect(view, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(contextualMenu(QPoint)));

    connect(copyAddressAction, SIGNAL(triggered()), this, SLOT(copyAddress()));
    connect(copyLabelAction, SIGNAL(triggered()), this, SLOT(copyLabel()));
    connect(copyAmountAction, SIGNAL(triggered()), this, SLOT(copyAmount()));
    connect(copyTxIDAction, SIGNAL(triggered()), this, SLOT(copyTxID()));
    connect(editLabelAction, SIGNAL(triggered()), this, SLOT(editLabel()));
    connect(showDetailsAction, SIGNAL(triggered()), this, SLOT(showDetails()));

    updateEmptyState();
    trxAmount(QString());
}

void TransactionView::setModel(WalletModel *newModel)
{
    model = newModel;
    if(!model)
    {
        transactionProxyModel = 0;
        exportButton->setEnabled(false);
        updateEmptyState();
        return;
    }

    transactionProxyModel = new TransactionFilterProxy(this);
    transactionProxyModel->setSourceModel(model->getTransactionTableModel());
    transactionProxyModel->setDynamicSortFilter(true);
    transactionProxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
    transactionProxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
    transactionProxyModel->setSortRole(Qt::EditRole);

    transactionView->setModel(transactionProxyModel);
    transactionView->sortByColumn(TransactionTableModel::Date, Qt::DescendingOrder);
    transactionView->setAlternatingRowColors(true);
    transactionView->setSelectionBehavior(QAbstractItemView::SelectRows);
    transactionView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    transactionView->setSortingEnabled(true);
    transactionView->verticalHeader()->hide();
    transactionView->horizontalHeader()->setStretchLastSection(false);
    transactionView->horizontalHeader()->setMinimumSectionSize(70);
    transactionView->horizontalHeader()->resizeSection(TransactionTableModel::Status, 24);
    transactionView->horizontalHeader()->resizeSection(TransactionTableModel::Date, 120);
    transactionView->horizontalHeader()->resizeSection(TransactionTableModel::Type, 110);
    transactionView->horizontalHeader()->setResizeMode(TransactionTableModel::ToAddress, QHeaderView::Stretch);
    transactionView->horizontalHeader()->resizeSection(TransactionTableModel::Narration, 140);
    transactionView->horizontalHeader()->resizeSection(TransactionTableModel::Amount, 100);

    connect(transactionView->selectionModel(), SIGNAL(selectionChanged(QItemSelection, QItemSelection)), this, SLOT(computeSum()));
    connect(transactionProxyModel, SIGNAL(rowsInserted(QModelIndex,int,int)), this, SLOT(updateEmptyState()));
    connect(transactionProxyModel, SIGNAL(rowsRemoved(QModelIndex,int,int)), this, SLOT(updateEmptyState()));
    connect(transactionProxyModel, SIGNAL(modelReset()), this, SLOT(updateEmptyState()));
    connect(transactionProxyModel, SIGNAL(layoutChanged()), this, SLOT(updateEmptyState()));

    exportButton->setEnabled(true);
    updateEmptyState();
}

void TransactionView::chooseDate(int idx)
{
    if(!transactionProxyModel)
        return;
    QDate current = QDate::currentDate();
    dateRangeWidget->setVisible(false);
    switch(dateWidget->itemData(idx).toInt())
    {
    case All:
        transactionProxyModel->setDateRange(
                TransactionFilterProxy::MIN_DATE,
                TransactionFilterProxy::MAX_DATE);
        break;
    case Today:
        transactionProxyModel->setDateRange(
                QDateTime(current),
                TransactionFilterProxy::MAX_DATE);
        break;
    case ThisWeek: {
        // Find last Monday.
        QDate startOfWeek = current.addDays(-(current.dayOfWeek()-1));
        transactionProxyModel->setDateRange(
                QDateTime(startOfWeek),
                TransactionFilterProxy::MAX_DATE);

        } break;
    case ThisMonth:
        transactionProxyModel->setDateRange(
                QDateTime(QDate(current.year(), current.month(), 1)),
                TransactionFilterProxy::MAX_DATE);
        break;
    case LastMonth:
        transactionProxyModel->setDateRange(
                QDateTime(QDate(current.year(), current.month()-1, 1)),
                QDateTime(QDate(current.year(), current.month(), 1)));
        break;
    case ThisYear:
        transactionProxyModel->setDateRange(
                QDateTime(QDate(current.year(), 1, 1)),
                TransactionFilterProxy::MAX_DATE);
        break;
    case Range:
        dateRangeWidget->setVisible(true);
        dateRangeChanged();
        break;
    }
    updateEmptyState();
}

void TransactionView::chooseType(int idx)
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setTypeFilter(
        typeWidget->itemData(idx).toInt());
    updateEmptyState();
}

void TransactionView::changedPrefix(const QString &prefix)
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setAddressPrefix(prefix);
    updateEmptyState();
}

void TransactionView::changedAmount(const QString &amount)
{
    if(!transactionProxyModel)
        return;
    qint64 amount_parsed = 0;
    if(BitcoinUnits::parse(model->getOptionsModel()->getDisplayUnit(), amount, &amount_parsed))
    {
        transactionProxyModel->setMinAmount(amount_parsed);
    }
    else
    {
        transactionProxyModel->setMinAmount(0);
    }
    updateEmptyState();
}

void TransactionView::exportClicked()
{
    if(!transactionProxyModel)
        return;

    // CSV is currently the only supported format.
    QString filename = GUIUtil::getSaveFileName(
            this,
            tr("Export Transaction Data"), QString(),
            tr("Comma separated file (*.csv)"));

    if (filename.isNull()) return;

    CSVModelWriter writer(filename);

    // name, column, role
    writer.setModel(transactionProxyModel);
    writer.addColumn(tr("Confirmed"), 0, TransactionTableModel::ConfirmedRole);
    writer.addColumn(tr("Date"), 0, TransactionTableModel::DateRole);
    writer.addColumn(tr("Type"), TransactionTableModel::Type, Qt::EditRole);
    writer.addColumn(tr("Label"), 0, TransactionTableModel::LabelRole);
    writer.addColumn(tr("Address"), 0, TransactionTableModel::AddressRole);
    writer.addColumn(tr("Amount"), 0, TransactionTableModel::FormattedAmountRole);
    writer.addColumn(tr("ID"), 0, TransactionTableModel::TxIDRole);

    if(!writer.write())
    {
        QMessageBox::critical(this, tr("Error exporting"), tr("Could not write to file %1.").arg(filename),
                              QMessageBox::Abort, QMessageBox::Abort);
    }
}

void TransactionView::contextualMenu(const QPoint &point)
{
    QModelIndex index = transactionView->indexAt(point);
    if(index.isValid())
    {
        contextMenu->exec(QCursor::pos());
    }
}

void TransactionView::copyAddress()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::AddressRole);
}

void TransactionView::copyLabel()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::LabelRole);
}

void TransactionView::copyAmount()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::FormattedAmountRole);
}

void TransactionView::copyTxID()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxIDRole);
}

void TransactionView::editLabel()
{
    if(!transactionView->selectionModel() || !model)
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows();
    if(!selection.isEmpty())
    {
        AddressTableModel *addressBook = model->getAddressTableModel();
        if(!addressBook)
            return;
        QString address = selection.at(0).data(TransactionTableModel::AddressRole).toString();
        if(address.isEmpty())
        {
            // If this transaction has no associated address, exit.
            return;
        }
        // Is address in address book? Address book can miss address when a transaction is
        // sent from outside the UI.
        int idx = addressBook->lookupAddress(address);
        if(idx != -1)
        {
            // Edit sending / receiving address.
            QModelIndex modelIdx = addressBook->index(idx, 0, QModelIndex());
            // Determine type of address, launch appropriate editor dialog type.
            QString type = modelIdx.data(AddressTableModel::TypeRole).toString();

            EditAddressDialog dlg(type == AddressTableModel::Receive
                                         ? EditAddressDialog::EditReceivingAddress
                                         : EditAddressDialog::EditSendingAddress,
                                  this);
            dlg.setModel(addressBook);
            dlg.loadRow(idx);
            dlg.exec();
        }
        else
        {
            // Add sending address.
            EditAddressDialog dlg(EditAddressDialog::NewSendingAddress,
                                  this);
            dlg.setModel(addressBook);
            dlg.setAddress(address);
            dlg.exec();
        }
    }
}

void TransactionView::showDetails()
{
    if(!transactionView->selectionModel())
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows();
    if(!selection.isEmpty())
    {
        TransactionDescDialog dlg(selection.at(0));
        dlg.exec();
    }
}

void TransactionView::computeSum()
{
    qint64 amount = 0;
    int nDisplayUnit = model->getOptionsModel()->getDisplayUnit();
    if (!transactionView->selectionModel())
    {
        emit trxAmount(QString());
        return;
    }
    QModelIndexList selection = transactionView->selectionModel()->selectedRows();

    foreach (QModelIndex index, selection) {
        amount += index.data(TransactionTableModel::AmountRole).toLongLong();
    }
    QString strAmount(BitcoinUnits::formatWithUnit(nDisplayUnit, amount, true));
    if (amount < 0) strAmount = "<span style='color:red;'>" + strAmount + "</span>";
    emit trxAmount(strAmount);
}

/** Update wallet with the sum of the selected transactions */
void TransactionView::trxAmount(QString amount)
{
    transactionSum->setText(amount);
}


QWidget *TransactionView::createDateRangeWidget()
{
    dateRangeWidget = new QFrame(this);
    dateRangeWidget->setFrameStyle(QFrame::NoFrame);
    QHBoxLayout *layout = new QHBoxLayout(dateRangeWidget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(new QLabel(tr("Range:")));

    dateFrom = new QDateTimeEdit(this);
    dateFrom->setDisplayFormat("dd/MM/yy");
    dateFrom->setCalendarPopup(true);
    dateFrom->setMinimumWidth(100);
    dateFrom->setDate(QDate::currentDate().addDays(-7));
    layout->addWidget(dateFrom);
    layout->addWidget(new QLabel(tr("to")));

    dateTo = new QDateTimeEdit(this);
    dateTo->setDisplayFormat("dd/MM/yy");
    dateTo->setCalendarPopup(true);
    dateTo->setMinimumWidth(100);
    dateTo->setDate(QDate::currentDate());
    layout->addWidget(dateTo);
    layout->addStretch();

    // Hide by default.
    dateRangeWidget->setVisible(false);

    // Notify on change.
    connect(dateFrom, SIGNAL(dateChanged(QDate)), this, SLOT(dateRangeChanged()));
    connect(dateTo, SIGNAL(dateChanged(QDate)), this, SLOT(dateRangeChanged()));

    return dateRangeWidget;
}

void TransactionView::dateRangeChanged()
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setDateRange(
            QDateTime(dateFrom->date()),
            QDateTime(dateTo->date()).addDays(1));
    updateEmptyState();
}

void TransactionView::focusTransaction(const QModelIndex &idx)
{
    if(!transactionProxyModel)
        return;
    contentStack->setCurrentWidget(tablePage);
    QModelIndex targetIdx = transactionProxyModel->mapFromSource(idx);
    transactionView->scrollTo(targetIdx);
    transactionView->setCurrentIndex(targetIdx);
    transactionView->setFocus();
}

void TransactionView::updateEmptyState()
{
    if(!contentStack || !tablePage || !emptyPage)
        return;

    if(!transactionProxyModel)
    {
        contentStack->setCurrentWidget(emptyPage);
        emptyStateTitle->setText(tr("No transactions yet."));
        emptyStateText->setText(tr("Transactions matching the current filters will appear here."));
        trxAmount(QString());
        return;
    }

    bool filtersActive = (dateWidget && dateWidget->currentData().toInt() != All) ||
                         (typeWidget && typeWidget->currentData().toInt() != TransactionFilterProxy::ALL_TYPES) ||
                         (addressWidget && !addressWidget->text().isEmpty()) ||
                         (amountWidget && !amountWidget->text().isEmpty());

    if(transactionProxyModel->rowCount() == 0)
    {
        contentStack->setCurrentWidget(emptyPage);
        if (filtersActive)
        {
            emptyStateTitle->setText(tr("No transactions match the current filters."));
            emptyStateText->setText(tr("Try clearing or widening the filters above."));
        }
        else
        {
            emptyStateTitle->setText(tr("No transactions yet."));
            emptyStateText->setText(tr("Transactions matching the current filters will appear here."));
        }
        if (transactionView && transactionView->selectionModel())
            transactionView->selectionModel()->clearSelection();
        trxAmount(QString());
    }
    else
    {
        contentStack->setCurrentWidget(tablePage);
    }
}

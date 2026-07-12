#include "addressbookpage.h"
#include "ui_addressbookpage.h"

#include "addresstablemodel.h"
#include "optionsmodel.h"
#include "walletmodel.h"
#include "bitcoingui.h"
#include "editaddressdialog.h"
#include "csvmodelwriter.h"
#include "guiutil.h"

#include <QSortFilterProxyModel>
#include <QClipboard>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QRegExp>
#include <QSettings>

#ifdef USE_QRCODE
#include "qrcodedialog.h"
#endif

class AddressBookSortFilterProxyModel final : public QSortFilterProxyModel
{
public:
    explicit AddressBookSortFilterProxyModel(const QString &type, QObject *parent = nullptr) :
        QSortFilterProxyModel(parent),
        m_type(type)
    {
        setDynamicSortFilter(true);
        setSortCaseSensitivity(Qt::CaseInsensitive);
        setFilterCaseSensitivity(Qt::CaseInsensitive);
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        const QAbstractItemModel *source = sourceModel();
        if (!source)
            return false;

        const QModelIndex labelIndex = source->index(row, AddressTableModel::Label, parent);
        if (source->data(labelIndex, AddressTableModel::TypeRole).toString() != m_type)
            return false;

        const QRegExp pattern = filterRegExp();
        if (pattern.isEmpty())
            return true;

        const QModelIndex addressIndex = source->index(row, AddressTableModel::Address, parent);
        return source->data(labelIndex).toString().contains(pattern) ||
               source->data(addressIndex).toString().contains(pattern);
    }

private:
    QString m_type;
};

AddressBookPage::AddressBookPage(Mode mode, Tabs tab, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::AddressBookPage),
    model(0),
    optionsModel(0),
    walletModel(0),
    mode(mode),
    tab(tab),
    proxyModel(0),
    contextMenu(0),
    deleteAction(0)
{
    ui->setupUi(this);

#ifdef Q_OS_MAC // Icons on push buttons are very uncommon on Mac
    ui->newAddressButton->setIcon(QIcon());
    ui->copyToClipboard->setIcon(QIcon());
    ui->deleteButton->setIcon(QIcon());
#endif

#ifndef USE_QRCODE
    ui->showQRCode->setVisible(false);
#endif

    if (tab == ReceivingTab) {
        ui->titleLabel->setVisible(true);
        ui->titleLabel->setText(tr("Receiving addresses"));
        ui->labelExplanation->setText(tr("These are your Innova addresses for receiving payments. Create a new address for each sender to keep payments organized."));
        ui->searchLineEdit->setVisible(true);
    } else {
        ui->titleLabel->setVisible(false);
        ui->searchLineEdit->setVisible(false);
    }

    switch(mode)
    {
    case ForSending:
        connect(ui->tableView, SIGNAL(doubleClicked(QModelIndex)), this, SLOT(accept()));
        ui->tableView->setToolTip(tr("Double-click to select address"));
        ui->tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ui->tableView->setFocus();
        break;
    case ForEditing:
        ui->buttonBox->setVisible(false);
        break;
    }
    switch(tab)
    {
    case SendingTab:
        ui->labelExplanation->setVisible(false);
        ui->deleteButton->setVisible(true);
        ui->signMessage->setVisible(false);
        break;
    case ReceivingTab:
        ui->deleteButton->setVisible(false);
        ui->signMessage->setVisible(true);

        // Add address type generation buttons for the Receive tab
        {
            QPushButton *btnNewShielded = new QPushButton(tr("New shielde&d address"), this);
            btnNewShielded->setToolTip(tr("Generate a new shielded address"));
            ui->horizontalLayout->insertWidget(1, btnNewShielded);
            connect(btnNewShielded, SIGNAL(clicked()), this, SLOT(onNewShieldedAddressClicked()));

            QPushButton *btnNewSP = new QPushButton(tr("New silent p&ayment address"), this);
            btnNewSP->setToolTip(tr("Generate a new Silent Payment address"));
            ui->horizontalLayout->insertWidget(2, btnNewSP);
            connect(btnNewSP, SIGNAL(clicked()), this, SLOT(onNewSPAddressClicked()));

            QPushButton *btnNewStaking = new QPushButton(tr("New s&taking address"), this);
            btnNewStaking->setToolTip(tr("Generate a new staking address for cold staking"));
            ui->horizontalLayout->insertWidget(3, btnNewStaking);
            connect(btnNewStaking, SIGNAL(clicked()), this, SLOT(onNewStakingAddressClicked()));
        }
        break;
    }

    connect(ui->searchLineEdit, SIGNAL(textChanged(QString)), this, SLOT(updateEmptyState()));

    // Context menu actions
    QAction *copyLabelAction = new QAction(tr("Copy &Label"), this);
    QAction *copyAddressAction = new QAction(ui->copyToClipboard->text(), this);
    QAction *editAction = new QAction(tr("&Edit"), this);
#ifdef USE_QRCODE
    QAction *showQRCodeAction = new QAction(ui->showQRCode->text(), this);
#endif
    QAction *signMessageAction = new QAction(ui->signMessage->text(), this);
    QAction *verifyMessageAction = new QAction(ui->verifyMessage->text(), this);
    deleteAction = new QAction(ui->deleteButton->text(), this);

    // Build context menu
    contextMenu = new QMenu();
    contextMenu->addAction(copyAddressAction);
    contextMenu->addAction(copyLabelAction);
    contextMenu->addAction(editAction);
    if(tab == SendingTab)
        contextMenu->addAction(deleteAction);
    contextMenu->addSeparator();
#ifdef USE_QRCODE
    contextMenu->addAction(showQRCodeAction);
#endif

    if(tab == ReceivingTab)
    {
        contextMenu->addAction(signMessageAction);
        if (mode == ForSending) {
            ui->tableView->setToolTip(tr("Double-click to select address"));
        } else {
            ui->tableView->setToolTip(tr("Double-click to edit label"));
        }
#ifdef USE_QRCODE
        // Show QR Code on double click when in receiving tab
        if (mode == ForEditing) {
            ui->tableView->setToolTip(tr("Double-click to edit label or show QR Code"));
            connect(ui->tableView, SIGNAL(doubleClicked(const QModelIndex&)), this, SLOT(onRowDoubleClicked(const QModelIndex&)));
        }
#endif
    }
    else if(tab == SendingTab) {
        contextMenu->addAction(verifyMessageAction);
    }

    // Connect signals for context menu actions
    connect(copyAddressAction, SIGNAL(triggered()), this, SLOT(on_copyToClipboard_clicked()));
    connect(copyLabelAction, SIGNAL(triggered()), this, SLOT(onCopyLabelAction()));
    connect(editAction, SIGNAL(triggered()), this, SLOT(onEditAction()));
    connect(deleteAction, SIGNAL(triggered()), this, SLOT(on_deleteButton_clicked()));
#ifdef USE_QRCODE
    connect(showQRCodeAction, SIGNAL(triggered()), this, SLOT(on_showQRCode_clicked()));
#endif
    connect(signMessageAction, SIGNAL(triggered()), this, SLOT(on_signMessage_clicked()));
    connect(verifyMessageAction, SIGNAL(triggered()), this, SLOT(on_verifyMessage_clicked()));

    connect(ui->tableView, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(contextualMenu(QPoint)));

    // Pass through accept action from button box
    connect(ui->buttonBox, SIGNAL(accepted()), this, SLOT(accept()));
}

void AddressBookPage::onRowDoubleClicked(const QModelIndex &idx)
{
        if(idx.column() > 0)
        {
            on_showQRCode_clicked();
        }
}

AddressBookPage::~AddressBookPage()
{
    delete ui;
}

void AddressBookPage::setModel(AddressTableModel *model)
{
    this->model = model;
    if(!model)
        return;

    const QString type = tab == ReceivingTab ? AddressTableModel::Receive : AddressTableModel::Send;
    proxyModel = new AddressBookSortFilterProxyModel(type, this);
    proxyModel->setSourceModel(model);
    proxyModel->setFilterWildcard(ui->searchLineEdit->text());

    ui->tableView->setModel(proxyModel);
    ui->tableView->sortByColumn(AddressTableModel::Label, Qt::AscendingOrder);
    ui->tableView->setTextElideMode(Qt::ElideMiddle);

    // Set column widths
    ui->tableView->horizontalHeader()->resizeSection(AddressTableModel::Address, 320);
    ui->tableView->horizontalHeader()->setSectionResizeMode(AddressTableModel::Label, QHeaderView::Stretch);
    ui->tableView->horizontalHeader()->setSectionResizeMode(AddressTableModel::Type, QHeaderView::ResizeToContents);

    connect(ui->tableView->selectionModel(), SIGNAL(selectionChanged(QItemSelection,QItemSelection)),
            this, SLOT(selectionChanged()));

    connect(ui->searchLineEdit, SIGNAL(textChanged(QString)), proxyModel, SLOT(setFilterWildcard(QString)));

    // Select row for newly created address
    connect(model, SIGNAL(rowsInserted(QModelIndex,int,int)),
            this, SLOT(selectNewAddress(QModelIndex,int,int)));
    connect(model, SIGNAL(rowsInserted(QModelIndex,int,int)),
            this, SLOT(updateEmptyState()));
    connect(model, SIGNAL(rowsRemoved(QModelIndex,int,int)),
            this, SLOT(updateEmptyState()));
    connect(model, SIGNAL(modelReset()),
            this, SLOT(updateEmptyState()));

    updateEmptyState();
    selectionChanged();
}

void AddressBookPage::setOptionsModel(OptionsModel *optionsModel)
{
    this->optionsModel = optionsModel;
}

void AddressBookPage::on_copyToClipboard_clicked()
{
    GUIUtil::copyEntryData(ui->tableView, AddressTableModel::Address);
}

void AddressBookPage::onCopyLabelAction()
{
    GUIUtil::copyEntryData(ui->tableView, AddressTableModel::Label);
}

void AddressBookPage::onEditAction()
{
    if(!ui->tableView->selectionModel())
        return;
    QModelIndexList indexes = ui->tableView->selectionModel()->selectedRows();
    if(indexes.isEmpty())
        return;

    EditAddressDialog dlg(
            tab == SendingTab ?
            EditAddressDialog::EditSendingAddress :
            EditAddressDialog::EditReceivingAddress);
    dlg.setModel(model);
    QModelIndex origIndex = proxyModel->mapToSource(indexes.at(0));
    dlg.loadRow(origIndex.row());
    dlg.exec();
}

void AddressBookPage::on_signMessage_clicked()
{
    QTableView *table = ui->tableView;
    QModelIndexList indexes = table->selectionModel()->selectedRows(AddressTableModel::Address);
    QString addr;

    foreach (QModelIndex index, indexes)
    {
        QVariant address = index.data();
        addr = address.toString();
    }

    emit signMessage(addr);
}

void AddressBookPage::on_verifyMessage_clicked()
{
    QTableView *table = ui->tableView;
    QModelIndexList indexes = table->selectionModel()->selectedRows(AddressTableModel::Address);
    QString addr;

    foreach (QModelIndex index, indexes)
    {
        QVariant address = index.data();
        addr = address.toString();
    }

    emit verifyMessage(addr);
}

void AddressBookPage::on_newAddressButton_clicked()
{
    if(!model)
        return;
    EditAddressDialog dlg(
            tab == SendingTab ?
            EditAddressDialog::NewSendingAddress :
            EditAddressDialog::NewReceivingAddress, this);
    dlg.setModel(model);
    if(dlg.exec())
    {
        newAddressToSelect = dlg.getAddress();
    }
}

void AddressBookPage::on_deleteButton_clicked()
{
    QTableView *table = ui->tableView;
    if(!table->selectionModel())
        return;
    QModelIndexList indexes = table->selectionModel()->selectedRows();
    if(!indexes.isEmpty())
    {
        table->model()->removeRow(indexes.at(0).row());
    }
}

void AddressBookPage::selectionChanged()
{
    // Set button states based on selected tab and selection
    QTableView *table = ui->tableView;
    if(!table->selectionModel())
        return;

    if(table->selectionModel()->hasSelection())
    {
        switch(tab)
        {
        case SendingTab:
            // In sending tab, allow deletion of selection
            ui->deleteButton->setEnabled(true);
            ui->deleteButton->setVisible(true);
            deleteAction->setEnabled(true);
            ui->signMessage->setEnabled(false);
            ui->signMessage->setVisible(false);
            ui->verifyMessage->setEnabled(true);
            ui->verifyMessage->setVisible(true);
            break;
        case ReceivingTab:
            // Deleting receiving addresses, however, is not allowed
            ui->deleteButton->setEnabled(false);
            ui->deleteButton->setVisible(false);
            deleteAction->setEnabled(false);
            ui->signMessage->setEnabled(true);
            ui->signMessage->setVisible(true);
            ui->verifyMessage->setEnabled(false);
            ui->verifyMessage->setVisible(false);
            break;
        }
        ui->copyToClipboard->setEnabled(true);
        ui->showQRCode->setEnabled(true);
    }
    else
    {
        ui->deleteButton->setEnabled(false);
        ui->showQRCode->setEnabled(false);
        ui->copyToClipboard->setEnabled(false);
        ui->signMessage->setEnabled(false);
        ui->verifyMessage->setEnabled(false);
    }
}

void AddressBookPage::updateEmptyState()
{
    if (!proxyModel)
        return;

    if (tab != ReceivingTab) {
        ui->contentStack->setCurrentWidget(ui->tablePage);
        return;
    }

    const bool hasRows = proxyModel->rowCount() > 0;
    ui->contentStack->setCurrentWidget(hasRows ? ui->tablePage : ui->emptyPage);
    if (!hasRows)
    {
        const bool hasSearch = !ui->searchLineEdit->text().trimmed().isEmpty();
        ui->emptyStateLabel->setText(hasSearch ?
            tr("No receiving addresses match your search.") :
            tr("No receiving addresses yet. Create a new address to get started."));
    }
}

void AddressBookPage::done(int retval)
{
    QTableView *table = ui->tableView;
    if(!table->selectionModel() || !table->model())
        return;
    // When this is a tab/widget and not a model dialog, ignore "done"
    if(mode == ForEditing)
        return;

    // Figure out which address was selected, and return it
    QModelIndexList indexes = table->selectionModel()->selectedRows(AddressTableModel::Address);

    foreach (QModelIndex index, indexes)
    {
        QVariant address = table->model()->data(index);
        returnValue = address.toString();
    }

    if(returnValue.isEmpty())
    {
        // If no address entry selected, return rejected
        retval = Rejected;
    }

    QDialog::done(retval);
}

void AddressBookPage::exportClicked()
{
    // CSV is currently the only supported format
    QString filename = GUIUtil::getSaveFileName(
            this,
            tr("Export Address Book Data"), QString(),
            tr("Comma separated file (*.csv)"));

    if (filename.isNull()) return;

    CSVModelWriter writer(filename);

    // name, column, role
    writer.setModel(proxyModel);
    writer.addColumn("Label", AddressTableModel::Label, Qt::EditRole);
    writer.addColumn("Address", AddressTableModel::Address, Qt::EditRole);

    if(!writer.write())
    {
        QMessageBox::critical(this, tr("Error exporting"), tr("Could not write to file %1.").arg(filename),
                              QMessageBox::Abort, QMessageBox::Abort);
    }
}

void AddressBookPage::on_showQRCode_clicked()
{
#ifdef USE_QRCODE
    QTableView *table = ui->tableView;
    QModelIndexList indexes = table->selectionModel()->selectedRows(AddressTableModel::Address);

    foreach (QModelIndex index, indexes)
    {
        QString address = index.data().toString(), label = index.sibling(index.row(), 0).data(Qt::EditRole).toString();

        QRCodeDialog *dialog = new QRCodeDialog(address, label, tab == ReceivingTab, this);
        if(optionsModel)
            dialog->setModel(optionsModel);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    }
#endif
}

void AddressBookPage::contextualMenu(const QPoint &point)
{
    QModelIndex index = ui->tableView->indexAt(point);
    if(index.isValid())
    {
        contextMenu->exec(QCursor::pos());
    }
}

void AddressBookPage::selectNewAddress(const QModelIndex &parent, int begin, int end)
{
    QModelIndex idx = proxyModel->mapFromSource(model->index(begin, AddressTableModel::Address, parent));
    if(idx.isValid() && (idx.data(Qt::EditRole).toString() == newAddressToSelect))
    {
        // Select row of newly created address, once
        ui->tableView->setFocus();
        ui->tableView->selectRow(idx.row());
        newAddressToSelect.clear();
    }
}

void AddressBookPage::setWalletModel(WalletModel *walletModel)
{
    this->walletModel = walletModel;
}

void AddressBookPage::onNewShieldedAddressClicked()
{
    if (!walletModel)
        return;

    bool ok;
    QString label = QInputDialog::getText(this, tr("New Shielded Address"),
        tr("Label for new shielded address (optional):"), QLineEdit::Normal, "", &ok);
    if (!ok) return;

    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid())
        return;

    QString newAddr = walletModel->getNewShieldedAddress();
    if (newAddr.isEmpty())
    {
        QMessageBox::warning(this, tr("Error"), tr("Failed to generate shielded address."));
        return;
    }

    // Save label for this z-address (persisted via QSettings)
    if (!label.isEmpty())
    {
        QSettings settings;
        settings.setValue("addrLabel/" + newAddr, label);
    }

    // Full refresh so z-address appears with correct "Shielded" type
    if (model) model->refresh();

    QApplication::clipboard()->setText(newAddr);
    QMessageBox::information(this, tr("New Shielded Address"),
        tr("Address copied to clipboard:\n\n%1").arg(newAddr));
}

void AddressBookPage::onNewSPAddressClicked()
{
    if (!walletModel)
        return;

    bool ok;
    QString label = QInputDialog::getText(this, tr("New Silent Payment Address"),
        tr("Label for new silent payment address (optional):"), QLineEdit::Normal, "", &ok);
    if (!ok) return;

    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid())
        return;

    QString newAddr = walletModel->getNewSilentPaymentAddress();
    if (newAddr.isEmpty())
    {
        QMessageBox::warning(this, tr("Error"), tr("Failed to generate silent payment address."));
        return;
    }

    if (!label.isEmpty())
    {
        QSettings settings;
        settings.setValue("addrLabel/" + newAddr, label);
    }
    if (model) model->refresh();

    QApplication::clipboard()->setText(newAddr);
    QMessageBox::information(this, tr("New Silent Payment Address"),
        tr("Address copied to clipboard:\n\n%1\n\n"
           "Share publicly. Each sender derives a unique one-time address.").arg(newAddr));
}

void AddressBookPage::onNewStakingAddressClicked()
{
    if (!walletModel)
        return;

    bool ok;
    QString label = QInputDialog::getText(this, tr("New Staking Address"),
        tr("Label for new staking address (optional):"), QLineEdit::Normal, "", &ok);
    if (!ok) return;

    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid())
        return;

    std::vector<std::string> params;
    QString error;
    QString result = walletModel->executeRPC("getnewstakingaddress", params, error);
    if (!error.isEmpty() || result.isEmpty())
    {
        QMessageBox::warning(this, tr("Error"),
            tr("Failed to generate staking address: %1").arg(error.isEmpty() ? "unknown error" : error));
        return;
    }

    result = result.trimmed();
    if (result.startsWith('"') && result.endsWith('"'))
        result = result.mid(1, result.length() - 2);

    // Add to address book so it appears in the receive list with "Staking" type
    if (walletModel && walletModel->getWallet())
    {
        CBitcoinAddress addr(result.toStdString());
        if (addr.IsValid())
        {
            std::string strLabel = label.isEmpty() ? "Staking Address" : label.toStdString();
            walletModel->getWallet()->SetAddressBookName(addr.Get(), strLabel);
        }
    }

    if (model) model->refresh();

    QApplication::clipboard()->setText(result);
    QMessageBox::information(this, tr("New Staking Address"),
        tr("Address copied to clipboard:\n\n%1\n\n"
           "Give this to a VPS staker for cold staking delegation.").arg(result));
}

#ifndef PRIVACYPAGE_H
#define PRIVACYPAGE_H

#include <QWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QGroupBox>
#include <QListWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QTabWidget>

class WalletModel;

class PrivacyPage : public QWidget
{
    Q_OBJECT

public:
    explicit PrivacyPage(QWidget *parent = 0);
    void setModel(WalletModel *model);

private slots:
    void onRefreshClicked();
    void onNewSPAddressClicked();
    void onCopySPAddressClicked();

private:
    WalletModel *model;

    // Silent Payment Addresses tab
    QListWidget *spAddressList;
    QPushButton *newSPAddressButton;
    QPushButton *copySPAddressButton;

    // Status
    QLabel *statusLabel;

    void setupUI();
    void refreshSPAddresses();
};

#endif // PRIVACYPAGE_H

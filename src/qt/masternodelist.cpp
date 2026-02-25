// Copyright (c) 2016-2025 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/masternodelist.h>
#include <qt/forms/ui_masternodelist.h>

#include <coins.h>
#include <evo/deterministicmns.h>
#include <evo/dmn_types.h>
#include <fs.h>
#include <rpc/client.h>
#include <saltedhasher.h>
#include <util/system.h>

#include <qt/clientmodel.h>
#include <qt/descriptiondialog.h>
#include <qt/guiutil.h>
#include <qt/guiutil_font.h>
#include <qt/walletmodel.h>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QWizard>
#include <QWizardPage>

#include <univalue.h>

#include <fstream>
#include <set>
#include <string_view>
#include <vector>

namespace {
constexpr int MASTERNODELIST_UPDATE_SECONDS{3};

QString ExtractRpcError(const UniValue& err)
{
    if (!err.isObject()) {
        return QString::fromStdString(err.write());
    }
    const UniValue& message = err.find_value("message");
    if (message.isStr()) {
        return QString::fromStdString(message.get_str());
    }
    return QString::fromStdString(err.write());
}

std::string_view TrimLeft(std::string_view line)
{
    const size_t first{line.find_first_not_of(" \t")};
    return first == std::string_view::npos ? std::string_view{} : line.substr(first);
}

class MasternodeSetupWizard final : public QWizard
{
public:
    explicit MasternodeSetupWizard(QWidget* parent, WalletModel* wallet_model);

protected:
    void accept() override;

private:
    enum class MnType {
        Regular,
        Evo,
    };

    WalletModel* m_wallet_model{nullptr};
    QComboBox* m_mn_type{nullptr};
    QLabel* m_collateral_label{nullptr};
    QLineEdit* m_ip{nullptr};
    QLineEdit* m_port{nullptr};
    QLineEdit* m_collateral_address{nullptr};
    QLineEdit* m_owner_address{nullptr};
    QLineEdit* m_voting_address{nullptr};
    QLineEdit* m_payout_address{nullptr};
    QLineEdit* m_fee_address{nullptr};
    QGroupBox* m_evo_group{nullptr};
    QLineEdit* m_evo_platform_node_id{nullptr};
    QLineEdit* m_evo_platform_p2p_addrs{nullptr};
    QLineEdit* m_evo_platform_https_addrs{nullptr};
    QLineEdit* m_bls_secret{nullptr};
    QLineEdit* m_bls_public{nullptr};
    QPlainTextEdit* m_summary{nullptr};
    bool m_restart_required{false};
    int m_review_page_id{-1};

    [[nodiscard]] MnType currentType() const
    {
        if (!m_mn_type) {
            return MnType::Regular;
        }
        const QVariant current_data{m_mn_type->currentData()};
        if (current_data.isValid()) {
            const int type_value{current_data.toInt()};
            if (type_value == static_cast<int>(MnType::Evo)) {
                return MnType::Evo;
            }
        }
        return MnType::Regular;
    }

    [[nodiscard]] std::string walletUri() const
    {
        if (!m_wallet_model) {
            return {};
        }
        const QByteArray encoded{QUrl::toPercentEncoding(m_wallet_model->getWalletName())};
        return "/wallet/" + std::string(encoded.constData(), encoded.length());
    }

    [[nodiscard]] QString serviceAddress() const
    {
        return QString("%1:%2").arg(m_ip->text().trimmed(), m_port->text().trimmed());
    }

    bool execWalletRpc(const std::string& method, const std::vector<std::string>& args, UniValue& out, QString& error) const;
    bool validateInput(QString& error) const;
    bool autoFillAddresses();
    bool generateBls();
    bool saveOperatorSecretToConfig(QString& error);
    bool registerMasternode(QString& txid, QString& error);
    void updateTypeUi();
    void updateSummary();
};

MasternodeSetupWizard::MasternodeSetupWizard(QWidget* parent, WalletModel* wallet_model) :
    QWizard(parent),
    m_wallet_model(wallet_model)
{
    setWindowTitle(tr("Masternode Setup Wizard"));
    setWizardStyle(QWizard::ModernStyle);
    setMinimumWidth(860);

    auto* intro_page = new QWizardPage();
    intro_page->setTitle(tr("Welcome"));
    auto* intro_layout = new QVBoxLayout(intro_page);
    auto* intro_text = new QLabel(tr("This wizard helps you create and register a masternode without typing long manual commands.\n\n"
                                     "Flow:\n"
                                     "1. Set network and payout addresses\n"
                                     "2. Generate BLS operator key\n"
                                     "3. Save operator key into smartiecoin.conf\n"
                                     "4. Register masternode transaction (ProTx)\n\n"
                                     "Tip: Use 'Auto-fill from wallet' to generate all required addresses."));
    intro_text->setWordWrap(true);
    intro_layout->addWidget(intro_text);
    addPage(intro_page);

    auto* details_page = new QWizardPage();
    details_page->setTitle(tr("Network and Addresses"));
    auto* details_layout = new QVBoxLayout(details_page);
    auto* details_form = new QFormLayout();
    m_mn_type = new QComboBox(details_page);
    m_mn_type->addItem(tr("Regular (15,000 SMT)"), static_cast<int>(MnType::Regular));
    m_mn_type->addItem(tr("Evo (75,000 SMT)"), static_cast<int>(MnType::Evo));
    details_form->addRow(tr("Masternode type"), m_mn_type);
    m_collateral_label = new QLabel(details_page);
    details_form->addRow(tr("Required collateral"), m_collateral_label);

    m_ip = new QLineEdit(details_page);
    m_ip->setPlaceholderText(tr("Public IP (example: 203.0.113.10)"));
    details_form->addRow(tr("Public IP"), m_ip);

    m_port = new QLineEdit(details_page);
    m_port->setText("8383");
    details_form->addRow(tr("Core P2P port"), m_port);

    m_collateral_address = new QLineEdit(details_page);
    details_form->addRow(tr("Collateral address"), m_collateral_address);
    m_owner_address = new QLineEdit(details_page);
    details_form->addRow(tr("Owner address"), m_owner_address);
    m_voting_address = new QLineEdit(details_page);
    details_form->addRow(tr("Voting address"), m_voting_address);
    m_payout_address = new QLineEdit(details_page);
    details_form->addRow(tr("Payout address"), m_payout_address);
    m_fee_address = new QLineEdit(details_page);
    details_form->addRow(tr("Fee source address"), m_fee_address);

    auto* auto_fill = new QPushButton(tr("Auto-fill from wallet"), details_page);
    details_form->addRow(QString{}, auto_fill);

    m_evo_group = new QGroupBox(tr("Evo extras"), details_page);
    auto* evo_form = new QFormLayout(m_evo_group);
    m_evo_platform_node_id = new QLineEdit(m_evo_group);
    m_evo_platform_node_id->setPlaceholderText(tr("Platform Node ID (hex)"));
    evo_form->addRow(tr("Platform Node ID"), m_evo_platform_node_id);
    m_evo_platform_p2p_addrs = new QLineEdit(m_evo_group);
    m_evo_platform_p2p_addrs->setText("22821");
    evo_form->addRow(tr("Platform P2P"), m_evo_platform_p2p_addrs);
    m_evo_platform_https_addrs = new QLineEdit(m_evo_group);
    m_evo_platform_https_addrs->setText("22822");
    evo_form->addRow(tr("Platform HTTPS"), m_evo_platform_https_addrs);

    details_layout->addLayout(details_form);
    details_layout->addWidget(m_evo_group);
    addPage(details_page);

    auto* bls_page = new QWizardPage();
    bls_page->setTitle(tr("Operator BLS Key"));
    auto* bls_layout = new QVBoxLayout(bls_page);
    auto* bls_text = new QLabel(tr("Generate operator key pair. The secret key will be saved to smartiecoin.conf as masternodeblsprivkey."));
    bls_text->setWordWrap(true);
    bls_layout->addWidget(bls_text);
    auto* bls_form = new QFormLayout();
    m_bls_secret = new QLineEdit(bls_page);
    m_bls_secret->setReadOnly(true);
    m_bls_public = new QLineEdit(bls_page);
    m_bls_public->setReadOnly(true);
    bls_form->addRow(tr("BLS secret"), m_bls_secret);
    bls_form->addRow(tr("BLS public"), m_bls_public);
    bls_layout->addLayout(bls_form);
    auto* generate_bls_btn = new QPushButton(tr("Generate BLS key"), bls_page);
    bls_layout->addWidget(generate_bls_btn, 0, Qt::AlignLeft);
    bls_layout->addStretch(1);
    addPage(bls_page);

    auto* review_page = new QWizardPage();
    review_page->setTitle(tr("Review and Create"));
    auto* review_layout = new QVBoxLayout(review_page);
    auto* review_text = new QLabel(tr("Review values below, then click Finish to create and register the masternode."));
    review_text->setWordWrap(true);
    review_layout->addWidget(review_text);
    m_summary = new QPlainTextEdit(review_page);
    m_summary->setReadOnly(true);
    review_layout->addWidget(m_summary);
    m_review_page_id = addPage(review_page);

    connect(m_mn_type, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { updateTypeUi(); });
    connect(auto_fill, &QPushButton::clicked, this, [this] {
        if (!autoFillAddresses()) {
            return;
        }
        updateSummary();
    });
    connect(generate_bls_btn, &QPushButton::clicked, this, [this] {
        if (!generateBls()) {
            return;
        }
        updateSummary();
    });
    connect(this, &QWizard::currentIdChanged, this, [this](int id) {
        if (id == m_review_page_id) {
            updateSummary();
        }
    });

    updateTypeUi();
}

bool MasternodeSetupWizard::execWalletRpc(const std::string& method, const std::vector<std::string>& args, UniValue& out, QString& error) const
{
    if (!m_wallet_model) {
        error = tr("Wallet is not loaded.");
        return false;
    }

    try {
        const UniValue params{RPCConvertValues(method, args)};
        out = m_wallet_model->node().executeRpc(method, params, walletUri());
        return true;
    } catch (const UniValue& rpc_error) {
        error = ExtractRpcError(rpc_error);
        return false;
    } catch (const std::exception& e) {
        error = QString::fromStdString(e.what());
        return false;
    }
}

bool MasternodeSetupWizard::validateInput(QString& error) const
{
    if (!m_wallet_model) {
        error = tr("Wallet is not loaded.");
        return false;
    }

    bool ok_port{false};
    const int port = m_port->text().trimmed().toInt(&ok_port);
    if (!ok_port || port < 1 || port > 65535) {
        error = tr("Invalid P2P port. Use a value between 1 and 65535.");
        return false;
    }

    if (m_ip->text().trimmed().isEmpty()) {
        error = tr("Public IP is required.");
        return false;
    }

    auto require_valid_address = [this, &error](const QLineEdit* field, const QString& label) {
        const QString value{field->text().trimmed()};
        if (value.isEmpty()) {
            error = tr("%1 is required.").arg(label);
            return false;
        }
        if (!m_wallet_model->validateAddress(value)) {
            error = tr("%1 is not a valid Smartiecoin address.").arg(label);
            return false;
        }
        return true;
    };

    if (!require_valid_address(m_collateral_address, tr("Collateral address"))) return false;
    if (!require_valid_address(m_owner_address, tr("Owner address"))) return false;
    if (!require_valid_address(m_voting_address, tr("Voting address"))) return false;
    if (!require_valid_address(m_payout_address, tr("Payout address"))) return false;
    if (!m_fee_address->text().trimmed().isEmpty() &&
        !require_valid_address(m_fee_address, tr("Fee source address"))) return false;

    if (m_bls_secret->text().trimmed().isEmpty() || m_bls_public->text().trimmed().isEmpty()) {
        error = tr("Generate BLS key pair before finishing.");
        return false;
    }

    if (currentType() == MnType::Evo) {
        if (m_evo_platform_node_id->text().trimmed().isEmpty()) {
            error = tr("Platform Node ID is required for Evo nodes.");
            return false;
        }
        if (m_evo_platform_p2p_addrs->text().trimmed().isEmpty()) {
            error = tr("Platform P2P value is required for Evo nodes.");
            return false;
        }
        if (m_evo_platform_https_addrs->text().trimmed().isEmpty()) {
            error = tr("Platform HTTPS value is required for Evo nodes.");
            return false;
        }
    }

    return true;
}

bool MasternodeSetupWizard::autoFillAddresses()
{
    QString error;
    UniValue result;

    auto get_new = [this, &error, &result](const std::string& label, QLineEdit* out) {
        if (!execWalletRpc("getnewaddress", {label}, result, error)) {
            return false;
        }
        if (!result.isStr()) {
            error = tr("Unexpected RPC response for getnewaddress.");
            return false;
        }
        out->setText(QString::fromStdString(result.get_str()));
        return true;
    };

    if (!get_new("mn_collateral", m_collateral_address) ||
        !get_new("mn_owner", m_owner_address) ||
        !get_new("mn_voting", m_voting_address) ||
        !get_new("mn_payout", m_payout_address)) {
        QMessageBox::warning(this, tr("MN Setup Wizard"), tr("Failed to auto-fill addresses: %1").arg(error));
        return false;
    }
    // Leave fee source empty by default so RPC can auto-select spendable coins.
    m_fee_address->clear();

    return true;
}

bool MasternodeSetupWizard::generateBls()
{
    QString error;
    UniValue result;
    if (!execWalletRpc("bls", {"generate"}, result, error)) {
        QMessageBox::warning(this, tr("MN Setup Wizard"), tr("Failed to generate BLS key: %1").arg(error));
        return false;
    }
    if (!result.isObject()) {
        QMessageBox::warning(this, tr("MN Setup Wizard"), tr("Unexpected RPC response for bls generate."));
        return false;
    }

    const UniValue& secret = result.find_value("secret");
    const UniValue& pub = result.find_value("public");
    if (!secret.isStr() || !pub.isStr()) {
        QMessageBox::warning(this, tr("MN Setup Wizard"), tr("RPC response does not include secret/public key."));
        return false;
    }

    m_bls_secret->setText(QString::fromStdString(secret.get_str()));
    m_bls_public->setText(QString::fromStdString(pub.get_str()));
    return true;
}

bool MasternodeSetupWizard::saveOperatorSecretToConfig(QString& error)
{
    const std::string secret{m_bls_secret->text().trimmed().toStdString()};
    if (secret.empty()) {
        error = tr("BLS secret key is empty.");
        return false;
    }

    const fs::path config_path{GetConfigFile(gArgs.GetPathArg("-conf", BITCOIN_CONF_FILENAME))};
    if (config_path.empty()) {
        error = tr("Unable to resolve smartiecoin.conf path.");
        return false;
    }
    const fs::path config_dir{config_path.parent_path()};
    if (!config_dir.empty()) {
        try {
            // TryCreateDirectories returns false when the directory already exists.
            // Treat that as success and only fail on exceptions or non-directory paths.
            TryCreateDirectories(config_dir);
        } catch (const fs::filesystem_error&) {
            error = tr("Unable to create config directory: %1").arg(GUIUtil::PathToQString(config_dir));
            return false;
        }
        if (!fs::exists(config_dir) || !fs::is_directory(config_dir)) {
            error = tr("Unable to create config directory: %1").arg(GUIUtil::PathToQString(config_dir));
            return false;
        }
    }

    std::vector<std::string> lines;
    if (fs::exists(config_path)) {
        std::ifstream in{config_path};
        if (!in.is_open()) {
            error = tr("Unable to read config file: %1").arg(GUIUtil::PathToQString(config_path));
            return false;
        }
        for (std::string line; std::getline(in, line); ) {
            lines.push_back(std::move(line));
        }
    }

    constexpr std::string_view key{"masternodeblsprivkey"};
    bool found_key{false};
    bool changed{false};
    for (std::string& line : lines) {
        const std::string_view trimmed{TrimLeft(line)};
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        if (trimmed.rfind(std::string(key) + "=", 0) == 0) {
            const std::string old_value{std::string(trimmed.substr(key.size() + 1))};
            found_key = true;
            changed = old_value != secret;
            line = std::string(key) + "=" + secret;
            break;
        }
    }

    if (!found_key) {
        lines.push_back(std::string(key) + "=" + secret);
        changed = true;
    }

    if (!changed) {
        return true;
    }

    fs::path tmp_path{config_path};
    tmp_path += ".tmp-mnsetup";
    std::ofstream out{tmp_path, std::ios::out | std::ios::trunc};
    if (!out.is_open()) {
        error = tr("Unable to write temporary config file: %1").arg(GUIUtil::PathToQString(tmp_path));
        return false;
    }

    for (const std::string& line : lines) {
        out << line << '\n';
    }
    out.close();

    if (!RenameOver(tmp_path, config_path)) {
        fs::remove(tmp_path);
        error = tr("Failed to update config file: %1").arg(GUIUtil::PathToQString(config_path));
        return false;
    }

    m_restart_required = true;
    return true;
}

bool MasternodeSetupWizard::registerMasternode(QString& txid, QString& error)
{
    const std::string service{serviceAddress().trimmed().toStdString()};
    const std::string fee_source{m_fee_address->text().trimmed().toStdString()};

    auto send_protx = [&](const bool include_fee_source, QString& out_error) -> bool {
        std::vector<std::string> args;

        if (currentType() == MnType::Regular) {
            args = {
                "register_fund",
                m_collateral_address->text().trimmed().toStdString(),
                service,
                m_owner_address->text().trimmed().toStdString(),
                m_bls_public->text().trimmed().toStdString(),
                m_voting_address->text().trimmed().toStdString(),
                "0",
                m_payout_address->text().trimmed().toStdString(),
            };
        } else {
            args = {
                "register_fund_evo",
                m_collateral_address->text().trimmed().toStdString(),
                service,
                m_owner_address->text().trimmed().toStdString(),
                m_bls_public->text().trimmed().toStdString(),
                m_voting_address->text().trimmed().toStdString(),
                "0",
                m_payout_address->text().trimmed().toStdString(),
                m_evo_platform_node_id->text().trimmed().toStdString(),
                m_evo_platform_p2p_addrs->text().trimmed().toStdString(),
                m_evo_platform_https_addrs->text().trimmed().toStdString(),
            };
        }

        if (include_fee_source) {
            args.push_back(fee_source);
            args.push_back("true");
        }

        UniValue result;
        if (!execWalletRpc("protx", args, result, out_error)) {
            return false;
        }
        if (!result.isStr()) {
            out_error = tr("Unexpected RPC response for ProTx registration.");
            return false;
        }

        txid = QString::fromStdString(result.get_str());
        return true;
    };

    const bool has_fee_source{!fee_source.empty()};
    QString first_error;
    if (send_protx(has_fee_source, first_error)) {
        return true;
    }

    // If a fee source was explicitly set but has no spendable UTXOs, retry in auto mode.
    if (has_fee_source) {
        const QString lowered = first_error.toLower();
        const bool retry_auto =
            lowered.contains("insufficient funds") ||
            lowered.contains("no funds at specified address");
        if (retry_auto) {
            QString retry_error;
            if (send_protx(/*include_fee_source=*/false, retry_error)) {
                return true;
            }
            error = retry_error;
            return false;
        }
    }

    error = first_error;
    return false;
}

void MasternodeSetupWizard::updateTypeUi()
{
    const bool evo{currentType() == MnType::Evo};
    m_collateral_label->setText(evo ? tr("75000 SMT") : tr("15000 SMT"));
    m_evo_group->setVisible(evo);
}

void MasternodeSetupWizard::updateSummary()
{
    QStringList lines;
    const bool evo{currentType() == MnType::Evo};

    lines << tr("Type: %1").arg(evo ? tr("Evo") : tr("Regular"));
    lines << tr("Collateral: %1").arg(evo ? tr("75000 SMT") : tr("15000 SMT"));
    lines << tr("Core service: %1").arg(serviceAddress());
    lines << tr("Collateral address: %1").arg(m_collateral_address->text().trimmed());
    lines << tr("Owner address: %1").arg(m_owner_address->text().trimmed());
    lines << tr("Voting address: %1").arg(m_voting_address->text().trimmed());
    lines << tr("Payout address: %1").arg(m_payout_address->text().trimmed());
    lines << tr("Fee source address: %1").arg(m_fee_address->text().trimmed().isEmpty() ? tr("(auto)") : m_fee_address->text().trimmed());
    lines << tr("BLS public key: %1").arg(m_bls_public->text().trimmed());

    if (evo) {
        lines << tr("Platform Node ID: %1").arg(m_evo_platform_node_id->text().trimmed());
        lines << tr("Platform P2P: %1").arg(m_evo_platform_p2p_addrs->text().trimmed());
        lines << tr("Platform HTTPS: %1").arg(m_evo_platform_https_addrs->text().trimmed());
    }

    const fs::path config_path{GetConfigFile(gArgs.GetPathArg("-conf", BITCOIN_CONF_FILENAME))};
    lines << tr("Config file: %1").arg(GUIUtil::PathToQString(config_path));
    lines << tr("Action on Finish: save masternodeblsprivkey and send ProTx registration.");
    m_summary->setPlainText(lines.join('\n'));
}

void MasternodeSetupWizard::accept()
{
    QString error;
    if (!validateInput(error)) {
        QMessageBox::warning(this, tr("MN Setup Wizard"), error);
        return;
    }

    auto unlock_context{m_wallet_model->requestUnlock(false)};
    if (!unlock_context.isValid()) {
        QMessageBox::warning(this, tr("MN Setup Wizard"), tr("Wallet unlock was canceled or failed."));
        return;
    }

    if (!saveOperatorSecretToConfig(error)) {
        QMessageBox::warning(this, tr("MN Setup Wizard"), error);
        return;
    }

    QString txid;
    if (!registerMasternode(txid, error)) {
        QMessageBox::warning(this, tr("MN Setup Wizard"), tr("Masternode registration failed: %1").arg(error));
        return;
    }

    QString success = tr("Masternode registration transaction sent.\n\nTxID:\n%1").arg(txid);
    if (m_restart_required) {
        success += tr("\n\nOperator key was written to smartiecoin.conf.\nRestart Smartiecoin Core so local masternode service uses the new key.");
    }
    QMessageBox::information(this, tr("MN Setup Wizard"), success);
    QWizard::accept();
}
} // anonymous namespace

bool MasternodeListSortFilterProxyModel::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const
{
    // "Type" filter
    if (m_type_filter != TypeFilter::All) {
        QModelIndex idx = sourceModel()->index(source_row, MasternodeModel::TYPE, source_parent);
        int type = sourceModel()->data(idx, Qt::EditRole).toInt();
        if (m_type_filter == TypeFilter::Regular && type != static_cast<int>(MnType::Regular)) {
            return false;
        }
        if (m_type_filter == TypeFilter::Evo && type != static_cast<int>(MnType::Evo)) {
            return false;
        }
    }

    // Banned filter
    if (m_hide_banned) {
        QModelIndex idx = sourceModel()->index(source_row, MasternodeModel::STATUS, source_parent);
        int banned = sourceModel()->data(idx, Qt::EditRole).toInt();
        if (banned != 0) {
            return false;
        }
    }

    // Text-matching filter
    if (const auto& regex = filterRegularExpression(); !regex.pattern().isEmpty()) {
        QModelIndex idx = sourceModel()->index(source_row, 0, source_parent);
        QString searchText = sourceModel()->data(idx, Qt::UserRole).toString();
        if (!searchText.contains(regex)) {
            return false;
        }
    }

    // "Owned" filter
    if (m_show_owned_only) {
        QModelIndex idx = sourceModel()->index(source_row, MasternodeModel::PROTX_HASH, source_parent);
        QString proTxHash = sourceModel()->data(idx, Qt::DisplayRole).toString();
        if (!m_owned_mns.contains(proTxHash)) {
            return false;
        }
    }

    return true;
}

MasternodeList::MasternodeList(QWidget* parent) :
    QWidget(parent),
    ui(new Ui::MasternodeList),
    m_proxy_model(new MasternodeListSortFilterProxyModel(this)),
    m_model(new MasternodeModel(this)),
    m_worker(new QObject),
    m_thread{new QThread(this)},
    m_timer{new QTimer(this)}
{
    ui->setupUi(this);

    GUIUtil::setFont({ui->label_count, ui->countLabel}, {GUIUtil::FontWeight::Bold, 14});

    // Set up proxy model
    m_proxy_model->setSourceModel(m_model);
    m_proxy_model->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy_model->setSortRole(Qt::EditRole);

    // Set up table view
    ui->tableViewMasternodes->setModel(m_proxy_model);
    ui->tableViewMasternodes->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->tableViewMasternodes->verticalHeader()->setVisible(false);

    // Set column widths
    auto* header = ui->tableViewMasternodes->horizontalHeader();
    header->setStretchLastSection(false);
    for (int col = 0; col < MasternodeModel::COUNT; ++col) {
        if (col == MasternodeModel::SERVICE) {
            header->setSectionResizeMode(col, QHeaderView::Stretch);
        } else {
            header->setSectionResizeMode(col, QHeaderView::ResizeToContents);
        }
    }

    // Hide ProTx Hash column (used for internal lookup)
    ui->tableViewMasternodes->setColumnHidden(MasternodeModel::PROTX_HASH, true);

    // Hide PoSe column by default (since "Hide banned" is checked by default)
    ui->tableViewMasternodes->setColumnHidden(MasternodeModel::POSE, true);

    ui->checkBoxOwned->setEnabled(false);
    ui->mnSetupWizardButton->setEnabled(false);

    contextMenuDIP3 = new QMenu(this);
    contextMenuDIP3->addAction(tr("Copy ProTx Hash"), this, &MasternodeList::copyProTxHash_clicked);
    contextMenuDIP3->addAction(tr("Copy Collateral Outpoint"), this, &MasternodeList::copyCollateralOutpoint_clicked);

    QMenu* filterMenu = contextMenuDIP3->addMenu(tr("Filter by"));
    filterMenu->addAction(tr("Collateral Address"), this, &MasternodeList::filterByCollateralAddress);
    filterMenu->addAction(tr("Payout Address"), this, &MasternodeList::filterByPayoutAddress);
    filterMenu->addAction(tr("Owner Address"), this, &MasternodeList::filterByOwnerAddress);
    filterMenu->addAction(tr("Voting Address"), this, &MasternodeList::filterByVotingAddress);

    connect(ui->tableViewMasternodes, &QTableView::customContextMenuRequested, this, &MasternodeList::showContextMenuDIP3);
    connect(ui->tableViewMasternodes, &QTableView::doubleClicked, this, &MasternodeList::extraInfoDIP3_clicked);
    connect(m_proxy_model, &QSortFilterProxyModel::rowsInserted, this, &MasternodeList::updateFilteredCount);
    connect(m_proxy_model, &QSortFilterProxyModel::rowsRemoved, this, &MasternodeList::updateFilteredCount);
    connect(m_proxy_model, &QSortFilterProxyModel::modelReset, this, &MasternodeList::updateFilteredCount);
    connect(m_proxy_model, &QSortFilterProxyModel::layoutChanged, this, &MasternodeList::updateFilteredCount);

    GUIUtil::updateFonts();

    // Background thread for calculating masternode list
    m_worker->moveToThread(m_thread);
    // Make sure executor object is deleted in its own thread
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();

    // Debounce timer to apply masternode list changes
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &MasternodeList::updateDIP3ListScheduled);
}

MasternodeList::~MasternodeList()
{
    m_timer->stop();
    m_thread->quit();
    m_thread->wait();
    delete ui;
}

void MasternodeList::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange) {
        QTimer::singleShot(0, m_model, &MasternodeModel::refreshIcons);
    }
}

void MasternodeList::setClientModel(ClientModel* model)
{
    this->clientModel = model;
    if (model) {
        connect(clientModel, &ClientModel::masternodeListChanged, this, &MasternodeList::handleMasternodeListChanged);
        m_timer->start(0);
    } else {
        m_timer->stop();
    }
}

void MasternodeList::setWalletModel(WalletModel* model)
{
    this->walletModel = model;
    ui->checkBoxOwned->setEnabled(model != nullptr);
    ui->mnSetupWizardButton->setEnabled(model != nullptr);
}

void MasternodeList::showContextMenuDIP3(const QPoint& point)
{
    QModelIndex index = ui->tableViewMasternodes->indexAt(point);
    if (index.isValid()) {
        contextMenuDIP3->exec(QCursor::pos());
    }
}

void MasternodeList::handleMasternodeListChanged()
{
    if (!clientModel || m_timer->isActive()) {
        // Too early or already processing, nothing to do
        return;
    }

    int delay{MASTERNODELIST_UPDATE_SECONDS * 1000};
    if (!clientModel->masternodeSync().isBlockchainSynced()) {
        // Currently syncing, reduce refreshes
        delay *= 10;
    }
    m_timer->start(delay);
}

void MasternodeList::updateDIP3ListScheduled()
{
    if (!clientModel || clientModel->node().shutdownRequested()) {
        return;
    }

    if (m_in_progress.exchange(true)) {
        // Already applying, re-arm for next attempt
        handleMasternodeListChanged();
        return;
    }

    QMetaObject::invokeMethod(m_worker, [this] {
        auto result = std::make_shared<CalcMnList>(calcMasternodeList());
        m_in_progress.store(false);
        QTimer::singleShot(0, this, [this, result] {
            if (result->m_valid) {
                setMasternodeList(std::move(*result));
            } else {
                // Something went wrong, try again
                handleMasternodeListChanged();
            }
        });
    });
}

MasternodeList::CalcMnList MasternodeList::calcMasternodeList() const
{
    CalcMnList ret;
    if (!clientModel || clientModel->node().shutdownRequested()) {
        return ret;
    }

    auto [mnList, pindex] = clientModel->getMasternodeList();
    if (!pindex) return ret;
    auto projectedPayees = mnList->getProjectedMNPayees(pindex);

    if (projectedPayees.empty() && mnList->getValidMNsCount() > 0) {
        // GetProjectedMNPayees failed to provide results for a list with valid mns.
        // Keep current list and let it try again later.
        return ret;
    }

    ret.m_list_height = mnList->getHeight();

    Uint256HashMap<CTxDestination> mapCollateralDests;
    mnList->forEachMN(/*only_valid=*/false, [&](const auto& dmn) {
        CTxDestination collateralDest;
        Coin coin;
        if (clientModel->node().getUnspentOutput(dmn.getCollateralOutpoint(), coin) &&
            ExtractDestination(coin.out.scriptPubKey, collateralDest)) {
            mapCollateralDests.emplace(dmn.getProTxHash(), collateralDest);
        }
    });

    Uint256HashMap<int> nextPayments;
    for (size_t i = 0; i < projectedPayees.size(); i++) {
        const auto& dmn = projectedPayees[i];
        nextPayments.emplace(dmn->getProTxHash(), ret.m_list_height + (int)i + 1);
    }

    mnList->forEachMN(/*only_valid=*/false, [&](const auto& dmn) {
        QString collateralStr = QObject::tr("UNKNOWN");
        auto collateralDestIt = mapCollateralDests.find(dmn.getProTxHash());
        if (collateralDestIt != mapCollateralDests.end()) {
            collateralStr = QString::fromStdString(EncodeDestination(collateralDestIt->second));
        }

        int nNextPayment = 0;
        auto nextPaymentIt = nextPayments.find(dmn.getProTxHash());
        if (nextPaymentIt != nextPayments.end()) {
            nNextPayment = nextPaymentIt->second;
        }

        ret.m_entries.push_back(std::make_unique<MasternodeEntry>(dmn, collateralStr, nNextPayment));
    });

    // Compute "owned" masternode hashes for the filter
    if (walletModel) {
        std::set<COutPoint> setOutpts;
        for (const auto& outpt : walletModel->wallet().listProTxCoins()) {
            setOutpts.emplace(outpt);
        }

        mnList->forEachMN(/*only_valid=*/false, [&](const auto& dmn) {
            bool fMyMasternode = setOutpts.count(dmn.getCollateralOutpoint()) ||
                                 walletModel->wallet().isSpendable(PKHash(dmn.getKeyIdOwner())) ||
                                 walletModel->wallet().isSpendable(PKHash(dmn.getKeyIdVoting())) ||
                                 walletModel->wallet().isSpendable(dmn.getScriptPayout()) ||
                                 walletModel->wallet().isSpendable(dmn.getScriptOperatorPayout());
            if (fMyMasternode) {
                ret.m_owned_mns.insert(QString::fromStdString(dmn.getProTxHash().ToString()));
            }
        });
    }

    ret.m_valid = true;
    return ret;
}

void MasternodeList::setMasternodeList(CalcMnList&& list)
{
    m_model->setCurrentHeight(list.m_list_height);
    m_model->reconcile(std::move(list.m_entries));

    if (walletModel) {
        m_proxy_model->setMyMasternodeHashes(std::move(list.m_owned_mns));
        if (ui->checkBoxOwned->isChecked()) {
            m_proxy_model->forceInvalidateFilter();
        }
    }

    updateFilteredCount();
}

void MasternodeList::updateFilteredCount()
{
    ui->countLabel->setText(QString::number(m_proxy_model->rowCount()));
}

void MasternodeList::on_filterText_textChanged(const QString& strFilterIn)
{
    m_proxy_model->setFilterRegularExpression(
        QRegularExpression(QRegularExpression::escape(strFilterIn), QRegularExpression::CaseInsensitiveOption));
    updateFilteredCount();
}

void MasternodeList::on_showMnConfButton_clicked()
{
    const fs::path masternode_conf_path{gArgs.GetDataDirNet() / "masternode.conf"};

    if (!fs::exists(masternode_conf_path)) {
        std::ofstream conf_file{masternode_conf_path, std::ios::out | std::ios::trunc};
        if (conf_file.is_open()) {
            conf_file << "# Smartiecoin masternode.conf\n";
            conf_file << "# Format:\n";
            conf_file << "# alias IP:port masternodeprivkey collateral_output_txid collateral_output_index\n";
            conf_file.close();
        }
    }

    if (!fs::exists(masternode_conf_path)) {
        QMessageBox::warning(this, tr("Masternode Config"), tr("Unable to open or create masternode.conf."));
        return;
    }

    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(GUIUtil::PathToQString(masternode_conf_path)))) {
#ifdef Q_OS_MACOS
        QProcess::startDetached("/usr/bin/open", QStringList{"-t", GUIUtil::PathToQString(masternode_conf_path)});
#else
        QMessageBox::warning(this, tr("Masternode Config"), tr("Unable to open masternode.conf with the system editor."));
#endif
    }
}

void MasternodeList::on_mnSetupWizardButton_clicked()
{
    if (!walletModel) {
        QMessageBox::warning(this, tr("MN Setup Wizard"), tr("Load a wallet first to use the masternode setup wizard."));
        return;
    }

    MasternodeSetupWizard wizard(this, walletModel);
    wizard.exec();
}

void MasternodeList::on_comboBoxType_currentIndexChanged(int index)
{
    if (index < 0 || index >= static_cast<int>(MasternodeListSortFilterProxyModel::TypeFilter::COUNT)) {
        return;
    }
    const auto index_enum{static_cast<MasternodeListSortFilterProxyModel::TypeFilter>(index)};
    ui->tableViewMasternodes->setColumnHidden(MasternodeModel::TYPE, index_enum != MasternodeListSortFilterProxyModel::TypeFilter::All);
    m_proxy_model->setTypeFilter(index_enum);
    m_proxy_model->forceInvalidateFilter();
    updateFilteredCount();
}

void MasternodeList::on_checkBoxOwned_stateChanged(int state)
{
    m_proxy_model->setShowOwnedOnly(state == Qt::Checked);
    m_proxy_model->forceInvalidateFilter();
    updateFilteredCount();
}

void MasternodeList::on_checkBoxHideBanned_stateChanged(int state)
{
    const bool hide_banned{state == Qt::Checked};
    ui->tableViewMasternodes->setColumnHidden(MasternodeModel::POSE, hide_banned);
    m_proxy_model->setHideBanned(hide_banned);
    m_proxy_model->forceInvalidateFilter();
    updateFilteredCount();
}

const MasternodeEntry* MasternodeList::GetSelectedEntry()
{
    if (!m_model) {
        return nullptr;
    }

    QItemSelectionModel* selectionModel = ui->tableViewMasternodes->selectionModel();
    if (!selectionModel) {
        return nullptr;
    }

    QModelIndexList selected = selectionModel->selectedRows();
    if (selected.count() == 0) {
        return nullptr;
    }

    // Map from proxy to source model
    return m_model->getEntryAt(m_proxy_model->mapToSource(selected.at(0)));
}

void MasternodeList::extraInfoDIP3_clicked()
{
    const auto* entry = GetSelectedEntry();
    if (!entry) {
        return;
    }

    auto* dialog = new DescriptionDialog(tr("Details for Masternode %1").arg(entry->proTxHash()), entry->toHtml(), /*parent=*/this);
    dialog->resize(1000, 500);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MasternodeList::copyProTxHash_clicked()
{
    const auto* entry = GetSelectedEntry();
    if (!entry) {
        return;
    }

    QApplication::clipboard()->setText(entry->proTxHash());
}

void MasternodeList::copyCollateralOutpoint_clicked()
{
    const auto* entry = GetSelectedEntry();
    if (!entry) {
        return;
    }

    QApplication::clipboard()->setText(entry->collateralOutpoint());
}

void MasternodeList::filterByCollateralAddress()
{
    const auto* entry = GetSelectedEntry();
    if (entry) {
        ui->filterText->setText(entry->collateralAddress());
    }
}

void MasternodeList::filterByPayoutAddress()
{
    const auto* entry = GetSelectedEntry();
    if (entry) {
        ui->filterText->setText(entry->payoutAddress());
    }
}

void MasternodeList::filterByOwnerAddress()
{
    const auto* entry = GetSelectedEntry();
    if (entry) {
        ui->filterText->setText(entry->ownerAddress());
    }
}

void MasternodeList::filterByVotingAddress()
{
    const auto* entry = GetSelectedEntry();
    if (entry) {
        ui->filterText->setText(entry->votingAddress());
    }
}

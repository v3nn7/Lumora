#include "collab/CollabUi.hpp"

#include "collab/CollabClient.hpp"
#include "collab/HostIdentity.hpp"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkInterface>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace lumora::collab {

namespace {

QString defaultName() {
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty()) user = qEnvironmentVariable("USERNAME");
    return sanitizeName(user.isEmpty() ? QStringLiteral("Guest") : user);
}

QLabel* hint(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setOpenExternalLinks(true);
    label->setStyleSheet(QStringLiteral("color: #8b93a1;"));
    return label;
}

QWidget* copyRow(const QString& caption, const QString& value) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* label = new QLabel(QStringLiteral("<span style='color:#8b93a1'>%1</span>&nbsp; <b>%2</b>")
                                 .arg(caption.toHtmlEscaped(), value.toHtmlEscaped()));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* copy = new QToolButton;
    copy->setText(QObject::tr("Copy"));
    copy->setToolTip(QObject::tr("Copy %1 to the clipboard").arg(value));
    QObject::connect(copy, &QToolButton::clicked, row, [value] { QApplication::clipboard()->setText(value); });
    layout->addWidget(label, 1);
    layout->addWidget(copy);
    return row;
}

QIcon dot(const QColor& color) {
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(color);
    p.setPen(Qt::NoPen);
    p.drawEllipse(1, 1, 10, 10);
    return QIcon(pixmap);
}

constexpr const char* kPlayitHelp =
    "<b>Over the internet with playit.gg</b><br>"
    "1. Install and run the <a href='https://playit.gg/download'>playit.gg agent</a> on this computer.<br>"
    "2. In your playit.gg dashboard add a tunnel of type <b>TCP+UDP</b> to local port <b>%1</b> "
    "(TCP only works too; camera positions then travel over TCP).<br>"
    "3. Paste the address playit.gg gives you below. The Session panel then shows it with the host "
    "certificate's fingerprint attached - share that link.";

} // namespace

// ---- Preferences -------------------------------------------------------------------------------

CollabPreferences CollabPreferences::load() {
    QSettings s;
    CollabPreferences p;
    p.name = sanitizeName(s.value(QStringLiteral("collab/name"), defaultName()).toString());
    const uint port = s.value(QStringLiteral("collab/port"), kDefaultPort).toUInt();
    p.port = port >= 1 && port <= 65535 ? static_cast<quint16>(port) : kDefaultPort;
    p.publicAddress = s.value(QStringLiteral("collab/publicAddress")).toString();
    p.recentAddresses = s.value(QStringLiteral("collab/recentAddresses")).toStringList();
    return p;
}

void CollabPreferences::save() const {
    QSettings s;
    s.setValue(QStringLiteral("collab/name"), name);
    s.setValue(QStringLiteral("collab/port"), port);
    s.setValue(QStringLiteral("collab/publicAddress"), publicAddress);
    s.setValue(QStringLiteral("collab/recentAddresses"), recentAddresses);
}

void CollabPreferences::rememberAddress(const QString& address) {
    recentAddresses.removeAll(address);
    recentAddresses.prepend(address);
    while (recentAddresses.size() > 8) recentAddresses.removeLast();
}

QStringList lanAddresses() {
    QStringList out;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
            (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() == QAbstractSocket::IPv4Protocol && !ip.isLoopback() && !ip.isLinkLocal()) {
                out << ip.toString();
            }
        }
    }
    out.removeDuplicates();
    return out;
}

// ---- Host ---------------------------------------------------------------------------------------

HostSessionDialog::HostSessionDialog(const QString& projectName, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Host a Live Session"));
    setMinimumWidth(520);
    const CollabPreferences prefs = CollabPreferences::load();

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(hint(tr("Share <b>%1</b> with others: everyone edits the same scene live and sees each "
                              "other's cameras. Imported models and textures are sent to whoever lacks them.")
                               .arg(projectName.toHtmlEscaped())));

    auto* form = new QFormLayout;
    name_ = new QLineEdit(prefs.name);
    name_->setMaxLength(kMaxNameLength);
    port_ = new QSpinBox;
    port_->setRange(1024, 65535);
    port_->setValue(prefs.port);
    password_ = new QLineEdit;
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(tr("optional - required to join when set"));
    form->addRow(tr("Your name"), name_);
    form->addRow(tr("Port (TCP + UDP)"), port_);
    form->addRow(tr("Password"), password_);
    layout->addLayout(form);

    auto* lanBox = new QGroupBox(tr("On your network"));
    auto* lanLayout = new QVBoxLayout(lanBox);
    lan_ = new QLabel;
    lan_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lan_->setWordWrap(true);
    lanLayout->addWidget(lan_);
    layout->addWidget(lanBox);

    auto* netBox = new QGroupBox(tr("Over the internet"));
    auto* netLayout = new QVBoxLayout(netBox);
    playitHint_ = hint(QString());
    netLayout->addWidget(playitHint_);
    auto* publicRow = new QHBoxLayout;
    public_ = new QLineEdit(prefs.publicAddress);
    public_->setPlaceholderText(tr("e.g. bright-fox.at.ply.gg:23145"));
    auto* open = new QPushButton(tr("Open playit.gg"));
    connect(open, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(QStringLiteral("https://playit.gg"))); });
    publicRow->addWidget(public_, 1);
    publicRow->addWidget(open);
    netLayout->addLayout(publicRow);
    layout->addWidget(netBox);

    error_ = new QLabel;
    error_->setStyleSheet(QStringLiteral("color: #ec6a5e;"));
    error_->hide();
    layout->addWidget(error_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Start Hosting"));
    connect(buttons, &QDialogButtonBox::accepted, this, &HostSessionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(port_, &QSpinBox::valueChanged, this, &HostSessionDialog::refreshAddresses);
    refreshAddresses();
}

void HostSessionDialog::refreshAddresses() {
    const QStringList ips = lanAddresses();
    QStringList lines;
    for (const QString& ip : ips) lines << QStringLiteral("<b>%1:%2</b>").arg(ip).arg(port_->value());
    lan_->setText(ips.isEmpty() ? tr("No network connection found - only this computer can join (127.0.0.1:%1).")
                                       .arg(port_->value())
                                : tr("Others on the same network join with: %1").arg(lines.join(QStringLiteral(", "))));
    playitHint_->setText(tr(kPlayitHelp).arg(port_->value()));
}

QString HostSessionDialog::displayName() const { return sanitizeName(name_->text()); }
quint16 HostSessionDialog::port() const { return static_cast<quint16>(port_->value()); }
QString HostSessionDialog::password() const { return password_->text(); }

QString HostSessionDialog::publicAddress() const {
    const auto parsed = parseAddress(public_->text());
    return parsed ? formatAddress(*parsed) : QString();
}

void HostSessionDialog::accept() {
    if (!public_->text().trimmed().isEmpty() && !parseAddress(public_->text())) {
        error_->setText(tr("The public address should look like host:port, e.g. bright-fox.at.ply.gg:23145."));
        error_->show();
        return;
    }
    CollabPreferences prefs = CollabPreferences::load();
    prefs.name = displayName();
    prefs.port = port();
    prefs.publicAddress = publicAddress();
    prefs.save();
    QDialog::accept();
}

// ---- Join ---------------------------------------------------------------------------------------

JoinSessionDialog::JoinSessionDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Join a Live Session"));
    setMinimumWidth(460);
    const CollabPreferences prefs = CollabPreferences::load();

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(hint(tr("Enter the address the host shared: a network address such as "
                              "<b>192.168.1.20:7777</b>, or a playit.gg address such as "
                              "<b>bright-fox.at.ply.gg:23145</b>.")));
    auto* form = new QFormLayout;
    address_ = new QComboBox;
    address_->setEditable(true);
    address_->addItems(prefs.recentAddresses);
    address_->setCurrentText(prefs.recentAddresses.value(0));
    address_->lineEdit()->setPlaceholderText(tr("host:port"));
    name_ = new QLineEdit(prefs.name);
    name_->setMaxLength(kMaxNameLength);
    password_ = new QLineEdit;
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(tr("if the host set one"));
    form->addRow(tr("Address"), address_);
    form->addRow(tr("Your name"), name_);
    form->addRow(tr("Password"), password_);
    layout->addLayout(form);

    error_ = new QLabel;
    error_->setStyleSheet(QStringLiteral("color: #ec6a5e;"));
    error_->hide();
    layout->addWidget(error_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Join"));
    connect(buttons, &QDialogButtonBox::accepted, this, &JoinSessionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

Address JoinSessionDialog::address() const { return parseAddress(address_->currentText()).value_or(Address{}); }
QString JoinSessionDialog::displayName() const { return sanitizeName(name_->text()); }
QString JoinSessionDialog::password() const { return password_->text(); }

void JoinSessionDialog::accept() {
    const auto parsed = parseAddress(address_->currentText());
    if (!parsed) {
        error_->setText(tr("Enter an address like 192.168.1.20:7777 or name.at.ply.gg:23145."));
        error_->show();
        return;
    }
    CollabPreferences prefs = CollabPreferences::load();
    prefs.name = displayName();
    prefs.rememberAddress(formatAddress(*parsed));
    prefs.save();
    QDialog::accept();
}

// ---- Panel --------------------------------------------------------------------------------------

CollabPanel::CollabPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    pages_ = new QStackedWidget;
    root->addWidget(pages_);

    // Idle page.
    auto* idle = new QWidget;
    auto* idleLayout = new QVBoxLayout(idle);
    idleLayout->addWidget(hint(tr("Edit this scene together with others on your network or over the internet.")));
    auto* host = new QPushButton(tr("Host Session…"));
    auto* join = new QPushButton(tr("Join Session…"));
    connect(host, &QPushButton::clicked, this, &CollabPanel::hostRequested);
    connect(join, &QPushButton::clicked, this, &CollabPanel::joinRequested);
    idleLayout->addWidget(host);
    idleLayout->addWidget(join);
    idleNote_ = hint(QString());
    idleNote_->hide();
    idleLayout->addWidget(idleNote_);
    idleLayout->addStretch(1);
    pages_->addWidget(idle);

    // Live page.
    auto* live = new QWidget;
    auto* liveLayout = new QVBoxLayout(live);
    liveLayout->setContentsMargins(0, 0, 0, 0);
    title_ = new QLabel;
    title_->setWordWrap(true);
    liveLayout->addWidget(title_);
    status_ = hint(QString());
    liveLayout->addWidget(status_);
    share_ = new QWidget;
    shareRows_ = new QVBoxLayout(share_);
    shareRows_->setContentsMargins(0, 0, 0, 0);
    shareRows_->setSpacing(2);
    liveLayout->addWidget(share_);
    people_ = new QListWidget;
    people_->setObjectName(QStringLiteral("collabParticipants"));
    people_->setMaximumHeight(130);
    people_->setSelectionMode(QAbstractItemView::NoSelection);
    liveLayout->addWidget(people_);
    chat_ = new QTextBrowser;
    chat_->setOpenLinks(false);
    liveLayout->addWidget(chat_, 1);
    message_ = new QLineEdit;
    message_->setPlaceholderText(tr("Message everyone…"));
    message_->setMaxLength(kMaxChatLength);
    connect(message_, &QLineEdit::returnPressed, this, [this] {
        if (client_ && !message_->text().trimmed().isEmpty()) {
            client_->sendChat(message_->text());
            message_->clear();
        }
    });
    liveLayout->addWidget(message_);
    auto* leave = new QPushButton(tr("Leave Session"));
    leave->setObjectName(QStringLiteral("collabLeave"));
    connect(leave, &QPushButton::clicked, this, &CollabPanel::leaveRequested);
    liveLayout->addWidget(leave);
    pages_->addWidget(live);
}

void CollabPanel::attach(CollabClient* client, std::optional<HostInfo> host) {
    if (client_) client_->disconnect(this);
    client_ = client;
    host_ = host;
    chat_->clear();
    pages_->setCurrentIndex(1);
    connect(client, &CollabClient::participantsChanged, this, &CollabPanel::refreshParticipants);
    connect(client, &CollabClient::participantsChanged, this, &CollabPanel::refreshStatus);
    connect(client, &CollabClient::presenceChanged, this, &CollabPanel::refreshParticipants);
    connect(client, &CollabClient::connectionInfoChanged, this, &CollabPanel::refreshStatus);
    connect(client, &CollabClient::chatReceived, this, [this](int from, const QString& text) {
        const auto& people = client_->participants();
        const auto it = people.find(from);
        appendChat(it != people.end() ? it->second.name : tr("Someone"),
                   it != people.end() ? it->second.color : QColor(Qt::gray), text);
    });

    while (QLayoutItem* item = shareRows_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (host_) {
        for (const QString& ip : lanAddresses()) {
            shareRows_->addWidget(copyRow(tr("LAN"), shareAddress(ip, host_->port, host_->fingerprint)));
        }
        if (const auto pub = parseAddress(host_->publicAddress)) {
            shareRows_->addWidget(copyRow(tr("Internet"), shareAddress(pub->host, pub->port, host_->fingerprint)));
        }
        if (host_->publicAddress.isEmpty()) {
            shareRows_->addWidget(hint(tr("For people outside your network, tunnel port %1 (TCP+UDP) with "
                                          "<a href='https://playit.gg'>playit.gg</a> and share its address.")
                                           .arg(host_->port)));
        }
    }
    share_->setVisible(shareRows_->count() > 0);
    const QString session = client->sessionName().toHtmlEscaped();
    title_->setText(host_ ? tr("<b>Hosting</b> %1").arg(session)
                          : tr("<b>Joined</b> %1 <span style='color:#8b93a1'>at %2</span>")
                                .arg(session, formatAddress(client->address()).toHtmlEscaped()));
    appendNote(host_ ? tr("Session started. Share an address below.") : tr("Connected."));
    refreshParticipants();
    refreshStatus();
}

void CollabPanel::detach(const QString& note) {
    if (client_) client_->disconnect(this);
    client_ = nullptr;
    host_.reset();
    pages_->setCurrentIndex(0);
    idleNote_->setText(note.toHtmlEscaped());
    idleNote_->setVisible(!note.isEmpty());
}

void CollabPanel::refreshParticipants() {
    people_->clear();
    if (!client_) return;
    for (const auto& [id, p] : client_->participants()) {
        QString text = p.name;
        if (id == client_->id()) text += tr(" (you)");
        if (!p.selection.isEmpty() && entityName) {
            const QString what = entityName(p.selection);
            if (!what.isEmpty()) text += QStringLiteral("  ·  ") + tr("on %1").arg(what);
        }
        auto* item = new QListWidgetItem(dot(p.color), text);
        people_->addItem(item);
    }
}

void CollabPanel::refreshStatus() {
    if (!client_) return;
    QStringList parts;
    parts << tr("%n person(s)", nullptr, static_cast<int>(client_->participants().size()));
    parts << (client_->udpActive() ? tr("cameras via UDP") : tr("cameras via TCP"));
    if (const auto ms = client_->latencyMs()) parts << tr("%1 ms").arg(*ms);
    parts << tr("🔒 %1").arg(client_->encryption());
    if (!client_->hostFingerprint().isEmpty()) {
        parts << tr("host certificate %1").arg(formatFingerprint(client_->hostFingerprint(), 4));
    }
    status_->setText(parts.join(QStringLiteral("  ·  ")));
}

void CollabPanel::appendChat(const QString& who, const QColor& color, const QString& text) {
    chat_->append(QStringLiteral("<span style='color:#8b93a1'>%1</span> <b style='color:%2'>%3</b>: %4")
                      .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")), color.name(),
                           who.toHtmlEscaped(), text.toHtmlEscaped()));
}

void CollabPanel::appendNote(const QString& text) {
    chat_->append(QStringLiteral("<i style='color:#8b93a1'>%1</i>").arg(text.toHtmlEscaped()));
}

} // namespace lumora::collab

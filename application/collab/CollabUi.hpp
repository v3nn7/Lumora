#pragma once

#include "common/ModuleExport.hpp"

#include "collab/Protocol.hpp"

#include <QDialog>
#include <QPointer>
#include <QWidget>

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTextBrowser;
class QVBoxLayout;

namespace lumora::collab {

class CollabClient;

/// Remembered collaboration settings (QSettings "collab/...").
struct CollabPreferences {
    QString name;
    quint16 port = kDefaultPort;
    QString publicAddress;          // e.g. the playit.gg tunnel address
    QStringList recentAddresses;    // most recent first

    [[nodiscard]] static CollabPreferences load();
    void save() const;
    void rememberAddress(const QString& address);
};

/// IPv4 addresses of this machine that others on the LAN can reach.
[[nodiscard]] QStringList lanAddresses();

/// Host a session: name, port, optional password, the LAN addresses to share, and how to make
/// the session reachable over the internet with a playit.gg tunnel.
class LUMORA_COLLAB_API HostSessionDialog final : public QDialog {
    Q_OBJECT
public:
    explicit HostSessionDialog(const QString& projectName, QWidget* parent = nullptr);

    [[nodiscard]] QString displayName() const;
    [[nodiscard]] quint16 port() const;
    [[nodiscard]] QString password() const;
    [[nodiscard]] QString publicAddress() const;  // validated, may be empty

private:
    void refreshAddresses();
    void accept() override;

    QLineEdit* name_ = nullptr;
    QSpinBox* port_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLineEdit* public_ = nullptr;
    QLabel* lan_ = nullptr;
    QLabel* playitHint_ = nullptr;
    QLabel* error_ = nullptr;
};

/// Join a session by LAN address or tunnel address.
class LUMORA_COLLAB_API JoinSessionDialog final : public QDialog {
    Q_OBJECT
public:
    explicit JoinSessionDialog(QWidget* parent = nullptr);

    [[nodiscard]] Address address() const;
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QString password() const;

private:
    void accept() override;

    QComboBox* address_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLabel* error_ = nullptr;
};

/// Editor dock content: who is in the session, how they are connected, the addresses to share
/// (when hosting), and a chat.
class LUMORA_COLLAB_API CollabPanel final : public QWidget {
    Q_OBJECT
public:
    explicit CollabPanel(QWidget* parent = nullptr);

    struct HostInfo {
        quint16 port = 0;
        QString publicAddress;
        QByteArray fingerprint;  // host certificate, appended to every shared address
    };
    /// Shows a running session. `host` is set when this editor hosts it.
    void attach(CollabClient* client, std::optional<HostInfo> host);
    /// Back to the idle page, with an optional note (e.g. why the session ended).
    void detach(const QString& note = {});
    /// Resolves an entity uuid to a display name (what someone has selected).
    std::function<QString(const QString&)> entityName;

signals:
    void hostRequested();
    void joinRequested();
    void leaveRequested();

private:
    void refreshParticipants();
    void refreshStatus();
    void appendChat(const QString& who, const QColor& color, const QString& text);
    void appendNote(const QString& text);

    QPointer<CollabClient> client_;
    std::optional<HostInfo> host_;
    QStackedWidget* pages_ = nullptr;
    QLabel* idleNote_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* status_ = nullptr;
    QWidget* share_ = nullptr;
    QVBoxLayout* shareRows_ = nullptr;
    QListWidget* people_ = nullptr;
    QTextBrowser* chat_ = nullptr;
    QLineEdit* message_ = nullptr;
};

} // namespace lumora::collab

/****************************************************************************
 *
 *   (c) 2009-2017 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/


#include "QGCMAVLink.h"
#include "QGCApplication.h"
#include "WiFiSetupComponentController.h"

// === WifiNetworksListModel ===


WifiNetworksListModel::WifiNetworksListModel(QObject* parent)
    : QAbstractListModel(parent)
{ }


QHash<int, QByteArray> WifiNetworksListModel::roleNames(void) const
{
    return {
        {NetworkRole::SsidRole,           "ssid"},
        {NetworkRole::EncryptionTypeRole, "encryptionType"}
    };
}


void WifiNetworksListModel::add(const WifiNetwork &network)
{
    using Parent = QAbstractListModel;

    Parent::beginInsertRows(QModelIndex(), rowCount(), rowCount());
    _networks.push_back(network);
    Parent::endInsertRows();
}


void WifiNetworksListModel::clear(void)
{
    using Parent = QAbstractListModel;

    Parent::beginRemoveRows(QModelIndex(), 0, rowCount());
    _networks.clear();
    Parent::endRemoveRows();
}


int WifiNetworksListModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return _networks.count();
}


QVariant WifiNetworksListModel::data(const QModelIndex &index, int role) const
{
    auto currentRow = index.row();

    if (currentRow < 0 || currentRow > _networks.count()) {
        return {};
    }

    auto const& network = _networks[currentRow];

    if (role == NetworkRole::SsidRole) {
        return network.ssid();
    } else if (role == NetworkRole::EncryptionTypeRole) {
        return network.encryptionType();
    }

    return {};
}


// === WifiSetupComponentController ===


namespace impl {
    static const int SSID_LENGTH             = sizeof(mavlink_wifi_network_add_t::ssid);
    static const int PASSWD_LENGTH           = sizeof(mavlink_wifi_network_add_t::password);
    static char const* EDGE_WIFI_SETTING_KEY = "WifiSetup/EdgeDefaultNetwork";

    QByteArray toRawString(QString string, int rawSize = SSID_LENGTH)
    {
        auto rawBytes = QByteArray(rawSize, '\0');
        string.squeeze();
        return rawBytes.replace(0, string.length(), string.toUtf8());
    }

    QString decodeString(char const* str, int size) {
        return QString::fromUtf8(QByteArray(str, size).append('\0'));
    }


    MAVLinkProtocol& mavlinkProtocol(void) {
        return *(qgcApp()->toolbox()->mavlinkProtocol());
    }


    mavlink_message_t makeAddNetworkMsg(QString const& ssid, int encryptType, QString const& passwd)
    {
        mavlink_message_t msg;
        mavlink_msg_wifi_network_add_pack(mavlinkProtocol().getSystemId(),
                                          mavlinkProtocol().getComponentId(),
                                          &msg, toRawString(ssid, SSID_LENGTH).data(),
                                          encryptType, toRawString(passwd,  PASSWD_LENGTH).data());
        return msg;
    }


    mavlink_message_t makeRemoveNetworkMsg(QString const& ssid)
    {
        mavlink_message_t msg;
        mavlink_msg_wifi_network_delete_pack(mavlinkProtocol().getSystemId(),
                                             mavlinkProtocol().getComponentId(),
                                             &msg, toRawString(ssid, SSID_LENGTH).data());
        return msg;
    }

    mavlink_message_t makeConnectNetworkMsg(QString const& ssid) {
        mavlink_message_t msg;
        mavlink_msg_wifi_network_connect_pack(mavlinkProtocol().getSystemId(),
                                              mavlinkProtocol().getComponentId(),
                                              &msg, impl::toRawString(ssid, SSID_LENGTH).data());
        return msg;
    }
}


int const WiFiSetupComponentController::_ssidMaxLength   = impl::SSID_LENGTH;
int const WiFiSetupComponentController::_passwdMaxLength = impl::PASSWD_LENGTH;


WiFiSetupComponentController::WiFiSetupComponentController()
    : _encryptTypeStrings{"OPEN", "WEP", "WPA", "WPA2"}
    , _edgeMode(EdgeMode::Undefined)
{
    QSettings appSettings;
    if (appSettings.contains(impl::EDGE_WIFI_SETTING_KEY)) {
        _defaultNetwork = appSettings.value(impl::EDGE_WIFI_SETTING_KEY).toString();
    }

    connect(_vehicle, &Vehicle::mavlinkWifiNetworkInformation,
            this,     &WiFiSetupComponentController::_handleWiFiNetworkInformation);

    connect(_vehicle, &Vehicle::mavlinkWifiStatus,
            this,     &WiFiSetupComponentController::_handleWifiStatus);

}


void WiFiSetupComponentController::bootAsAccessPoint()
{
    _vehicle->sendMavCommand(MAV_COMP_ID_WIFI, MAV_CMD_WIFI_START_AP, true, 1);
}


void WiFiSetupComponentController::bootAsClient(QString const& name)
{
    _vehicle->sendMessageOnLink(_vehicle->priorityLink(),
                                impl::makeConnectNetworkMsg(name));
}


void WiFiSetupComponentController::saveNetworkToEdge(QString const& name, int type, QString const& psw)
{
    qInfo() << "debug add";
    _vehicle->sendMessageOnLink(_vehicle->priorityLink(),
                                impl::makeAddNetworkMsg(name, type, psw));
    updateNetwokrsList();
}


void WiFiSetupComponentController::removeNetworkFromEdge(QString const& name)
{
    qInfo() << "debug delete";
    _vehicle->sendMessageOnLink(_vehicle->priorityLink(),
                                impl::makeRemoveNetworkMsg(name));
    updateNetwokrsList();
}


void WiFiSetupComponentController::setDefaultNetwork(const QString &netwkName)
{
    QSettings appSettings;

    appSettings.setValue(impl::EDGE_WIFI_SETTING_KEY, netwkName);
    _defaultNetwork = netwkName;

    appSettings.sync();

    emit defaultNetworkChanged();
}


void WiFiSetupComponentController::requestWifiStatus(void)
{
    _vehicle->sendMavCommand(MAV_COMP_ID_WIFI, MAV_CMD_REQUEST_WIFI_STATUS, false, 1);
}


void WiFiSetupComponentController::updateNetwokrsList()
{
    _savedNetworks.clear();
    _vehicle->sendMavCommand(MAV_COMP_ID_WIFI, MAV_CMD_REQUEST_WIFI_NETWORKS, false, 1);
}


void WiFiSetupComponentController::_handleWifiStatus(mavlink_message_t message)
{
    mavlink_wifi_status_t wifiStatus;
    mavlink_msg_wifi_status_decode(&message, &wifiStatus);

    _edgeMode = static_cast<EdgeMode>(wifiStatus.state);
    auto ssid = impl::decodeString(wifiStatus.ssid, sizeof(wifiStatus.ssid));

    _activeNetwork = _edgeMode == EdgeMode::Client ? ssid : QStringLiteral("");

    emit edgeModeChanged();
    emit activeNetworkChanged();
}


void WiFiSetupComponentController::_handleWiFiNetworkInformation(mavlink_message_t message)
{
    mavlink_wifi_network_information_t wifiNetworkInfo;
    mavlink_msg_wifi_network_information_decode(&message, &wifiNetworkInfo);

    if (wifiNetworkInfo.type == 0) {
        auto ssid = impl::decodeString(wifiNetworkInfo.ssid,
                                       sizeof(wifiNetworkInfo.ssid));

        _savedNetworks << ssid;
        emit savedNetworksUpdated();
    }
}

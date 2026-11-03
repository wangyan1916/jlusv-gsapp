#pragma once

#include <QMainWindow>
#include <QQuickWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QTextEdit>
#include "network/gs_client.h"
#include "map/map_bridge.h"
#include "map/local_tile_server.h"

namespace jlusv {

class AppWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit AppWindow(QWidget *parent = nullptr);
    ~AppWindow() override = default;

private slots:
    void onConnectClicked();
    void onConnectionStatusChanged(bool connected);
    void onTelemetryUpdated(const UsvTelemetry &telemetry);
    void onMissionResult(bool success, const QString &message);
    void onPublishWaypointsClicked();
    void onEmergencyStopClicked();
    void onClearWaypointsClicked();
    void onUndoWaypointClicked();
    void onCopyWaypointsJson();
    void onCopyWaypointsText();
    void onPasteWaypointsClicked();
    void onGcj02Toggled(bool checked);

private:
    void setupUi();
    void logMessage(const QString &msg);
    void refreshControls();
    void refreshPositionDetails();
    int selectedWork() const;

    LocalTileServer *m_tileServer;
    GsClient *m_client;
    MapBridge *m_bridge;

    QQuickWidget *m_quickWidget;

    // UI 组件
    QLineEdit *m_ipEdit;
    QLineEdit *m_pwdEdit;
    QPushButton *m_connectBtn;
    QLabel *m_statusLabel;
    QComboBox *m_targetSelect;

    QLabel *m_uidLabel;
    QLabel *m_modeLabel;
    QLabel *m_rtkLabel;
    QLabel *m_posLabel;
    QLabel *m_positionSourceLabel;
    QLabel *m_diffAgeLabel;
    QLabel *m_headingLabel;
    QLabel *m_cogLabel;
    QLabel *m_speedLabel;
    QLabel *m_batteryLabel;
    QLabel *m_gsPosLabel;
    QLabel *m_gsSourceLabel;
    QLabel *m_gsAgeLabel;
    QLabel *m_relativeDistanceLabel;
    QLabel *m_relativeOffsetLabel;

    QCheckBox *m_gcj02Check;
    QLabel *m_waypointCountLabel;
    QDoubleSpinBox *m_speedSpin;
    QComboBox *m_ownerSelect;
    QComboBox *m_taskSelect;
    QPushButton *m_applyModeBtn;
    QLabel *m_modeHint;
    QDoubleSpinBox *m_headingSpin;
    QDoubleSpinBox *m_throttleSpin;
    QSpinBox *m_ttlSpin;
    QCheckBox *m_uniformSpeed;
    QLabel *m_executionLabel;
    QPushButton *m_publishBtn;
    QPushButton *m_stopBtn;
    QPushButton *m_clearBtn;
    QPushButton *m_undoBtn;
    QToolButton *m_copyBtn;
    QPushButton *m_pasteBtn;
    QTextEdit *m_logEdit;
};

} // namespace jlusv

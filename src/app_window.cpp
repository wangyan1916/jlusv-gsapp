#include "app_window.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QQmlContext>
#include <QDateTime>
#include <QMessageBox>

namespace jlusv {

AppWindow::AppWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_tileServer(new LocalTileServer(18080, this))
    , m_client(new GsClient(this))
    , m_bridge(new MapBridge(this))
{
    m_tileServer->start();
    setupUi();

    connect(m_client, &GsClient::connectionStatusChanged, this, &AppWindow::onConnectionStatusChanged);
    connect(m_client, &GsClient::telemetryUpdated, this, &AppWindow::onTelemetryUpdated);
    connect(m_client, &GsClient::missionResult, this, &AppWindow::onMissionResult);
    connect(m_client, &GsClient::authStatusChanged, this, [this](bool ok, const QString &msg) {
        if (ok) {
            m_statusLabel->setText("状态: 已连接 (安全认证成功)");
            m_statusLabel->setStyleSheet("color: green; font-weight: bold;");
            logMessage("地面站安全认证通过 (已获取写入控制会话)");
        } else {
            m_statusLabel->setText("状态: 已连接 (认证失败)");
            m_statusLabel->setStyleSheet("color: #E65100; font-weight: bold;");
            logMessage("地面站安全认证未通过: " + msg);
        }
    });

    connect(m_client, &GsClient::telemetryUpdated, m_bridge, &MapBridge::updateTelemetry);
    connect(m_bridge, &MapBridge::waypointsChanged, this, [this]() {
        m_waypointCountLabel->setText(QString("航点数: %1 / 16").arg(m_bridge->rawWaypoints().size()));
    });
}

void AppWindow::setupUi() {
    setWindowTitle("JLUSV 地面站循迹系统 (Qt 5.15.3 C++17)");
    resize(1280, 800);

    auto *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    auto *mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    // 1. 左侧/中央：QML 卫星地图
    m_quickWidget = new QQuickWidget(this);
    m_quickWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_quickWidget->rootContext()->setContextProperty("mapBridge", m_bridge);
    m_quickWidget->setSource(QUrl("qrc:/MainMap.qml"));
    mainLayout->addWidget(m_quickWidget, 3);

    // 2. 右侧：控制与遥测面板
    auto *panelWidget = new QWidget(this);
    panelWidget->setMaximumWidth(380);
    auto *panelLayout = new QVBoxLayout(panelWidget);
    panelLayout->setContentsMargins(0, 0, 0, 0);

    // 通信连接卡片
    auto *connGroup = new QGroupBox("地面站网关连接", panelWidget);
    auto *connLayout = new QGridLayout(connGroup);
    m_ipEdit = new QLineEdit("http://192.168.188.2", connGroup);
    m_pwdEdit = new QLineEdit("jlusv", connGroup);
    m_pwdEdit->setEchoMode(QLineEdit::Password);
    m_connectBtn = new QPushButton("连接", connGroup);
    m_statusLabel = new QLabel("状态: 未连接", connGroup);
    m_statusLabel->setStyleSheet("color: red; font-weight: bold;");
    connLayout->addWidget(new QLabel("网关地址:"), 0, 0);
    connLayout->addWidget(m_ipEdit, 0, 1, 1, 2);
    connLayout->addWidget(new QLabel("控制密码:"), 1, 0);
    connLayout->addWidget(m_pwdEdit, 1, 1);
    connLayout->addWidget(m_connectBtn, 1, 2);
    connLayout->addWidget(m_statusLabel, 2, 0, 1, 3);
    panelLayout->addWidget(connGroup);

    connect(m_connectBtn, &QPushButton::clicked, this, &AppWindow::onConnectClicked);

    // 船只状态卡片
    auto *statusGroup = new QGroupBox("船端遥测数据 (WGS-84 RTK)", panelWidget);
    auto *statusLayout = new QGridLayout(statusGroup);
    m_uidLabel = new QLabel("--", statusGroup);
    m_rtkLabel = new QLabel("--", statusGroup);
    m_posLabel = new QLabel("--", statusGroup);
    m_headingLabel = new QLabel("--", statusGroup);
    m_speedLabel = new QLabel("--", statusGroup);
    m_batteryLabel = new QLabel("--", statusGroup);

    statusLayout->addWidget(new QLabel("船只 UID:"), 0, 0);
    statusLayout->addWidget(m_uidLabel, 0, 1);
    statusLayout->addWidget(new QLabel("RTK 解状态:"), 1, 0);
    statusLayout->addWidget(m_rtkLabel, 1, 1);
    statusLayout->addWidget(new QLabel("经纬度:"), 2, 0);
    statusLayout->addWidget(m_posLabel, 2, 1);
    statusLayout->addWidget(new QLabel("船艏向:"), 3, 0);
    statusLayout->addWidget(m_headingLabel, 3, 1);
    statusLayout->addWidget(new QLabel("对地航速:"), 4, 0);
    statusLayout->addWidget(m_speedLabel, 4, 1);
    statusLayout->addWidget(new QLabel("电池电压:"), 5, 0);
    statusLayout->addWidget(m_batteryLabel, 5, 1);
    panelLayout->addWidget(statusGroup);

    // 地图与坐标系设置
    auto *mapSettingGroup = new QGroupBox("底图纠偏设置", panelWidget);
    auto *mapSettingLayout = new QVBoxLayout(mapSettingGroup);
    m_gcj02Check = new QCheckBox("开启 GCJ-02 卫星图坐标逆纠偏 (国内适用)", mapSettingGroup);
    m_gcj02Check->setChecked(true);
    connect(m_gcj02Check, &QCheckBox::toggled, this, &AppWindow::onGcj02Toggled);
    mapSettingLayout->addWidget(m_gcj02Check);
    panelLayout->addWidget(mapSettingGroup);

    // 航点任务卡片
    auto *missionGroup = new QGroupBox("循迹航线发布", panelWidget);
    auto *missionLayout = new QGridLayout(missionGroup);
    m_waypointCountLabel = new QLabel("航点数: 0 / 16 (点击地图添加)", missionGroup);
    m_speedSpin = new QDoubleSpinBox(missionGroup);
    m_speedSpin->setRange(0.2, 3.0);
    m_speedSpin->setSingleStep(0.1);
    m_speedSpin->setValue(1.2);
    m_speedSpin->setSuffix(" m/s");

    m_publishBtn = new QPushButton("发布循迹任务", missionGroup);
    m_publishBtn->setStyleSheet("background-color: #2E7D32; color: white; font-weight: bold; height: 32px;");
    m_stopBtn = new QPushButton("紧急制动 / 停船", missionGroup);
    m_stopBtn->setStyleSheet("background-color: #C62828; color: white; font-weight: bold; height: 32px;");
    m_undoBtn = new QPushButton("撤销航点", missionGroup);
    m_clearBtn = new QPushButton("清空航线", missionGroup);

    missionLayout->addWidget(m_waypointCountLabel, 0, 0, 1, 2);
    missionLayout->addWidget(new QLabel("期望巡航速度:"), 1, 0);
    missionLayout->addWidget(m_speedSpin, 1, 1);
    missionLayout->addWidget(m_undoBtn, 2, 0);
    missionLayout->addWidget(m_clearBtn, 2, 1);
    missionLayout->addWidget(m_publishBtn, 3, 0, 1, 2);
    missionLayout->addWidget(m_stopBtn, 4, 0, 1, 2);
    panelLayout->addWidget(missionGroup);

    connect(m_publishBtn, &QPushButton::clicked, this, &AppWindow::onPublishWaypointsClicked);
    connect(m_stopBtn, &QPushButton::clicked, this, &AppWindow::onEmergencyStopClicked);
    connect(m_clearBtn, &QPushButton::clicked, this, &AppWindow::onClearWaypointsClicked);
    connect(m_undoBtn, &QPushButton::clicked, this, &AppWindow::onUndoWaypointClicked);

    // 系统日志框
    auto *logGroup = new QGroupBox("操作日志", panelWidget);
    auto *logLayout = new QVBoxLayout(logGroup);
    m_logEdit = new QTextEdit(logGroup);
    m_logEdit->setReadOnly(true);
    logLayout->addWidget(m_logEdit);
    panelLayout->addWidget(logGroup, 1);

    mainLayout->addWidget(panelWidget, 1);

    logMessage("地面站启动完成。请配置网关地址并连接。");
}

void AppWindow::logMessage(const QString &msg) {
    const QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_logEdit->append(QString("[%1] %2").arg(timestamp, msg));
}

void AppWindow::onConnectClicked() {
    m_client->setBaseUrl(m_ipEdit->text());
    m_client->setPassword(m_pwdEdit->text());
    m_client->startPolling(500);
    logMessage("开始轮询地面站网关: " + m_client->baseUrl());
    logMessage("正在向网关请求安全认证 (POST /api/v1/auth/login)...");
    m_client->login();
}

void AppWindow::onConnectionStatusChanged(bool connected) {
    if (connected) {
        if (m_client->isAuthenticated()) {
            m_statusLabel->setText("状态: 已连接 (安全认证成功)");
            m_statusLabel->setStyleSheet("color: green; font-weight: bold;");
        } else {
            m_statusLabel->setText("状态: 已连接地面站");
            m_statusLabel->setStyleSheet("color: #1565C0; font-weight: bold;");
        }
        logMessage("地面站 HTTP 通信正常。");
    } else {
        m_statusLabel->setText("状态: 通信中断");
        m_statusLabel->setStyleSheet("color: red; font-weight: bold;");
        logMessage("地面站 HTTP 通信断开，正在重试...");
    }
}

void AppWindow::onTelemetryUpdated(const UsvTelemetry &t) {
    m_uidLabel->setText(t.uid.isEmpty() ? "--" : t.uid);
    m_rtkLabel->setText(m_bridge->rtkStateText());
    m_posLabel->setText(QString("%1, %2").arg(t.latDeg, 0, 'f', 6).arg(t.lonDeg, 0, 'f', 6));
    m_headingLabel->setText(QString("%1°").arg(t.headingDeg, 0, 'f', 1));
    m_speedLabel->setText(QString("%1 m/s").arg(t.sogMps, 0, 'f', 2));
    m_batteryLabel->setText(QString("%1 V").arg(t.batteryV, 0, 'f', 2));
}

void AppWindow::onGcj02Toggled(bool checked) {
    m_bridge->setUseGcj02(checked);
    logMessage(QString("底图坐标逆纠偏模式: %1").arg(checked ? "启用 (GCJ-02)" : "禁用 (真实 WGS-84)"));
}

void AppWindow::onPublishWaypointsClicked() {
    auto wps = m_bridge->rawWaypoints();
    if (wps.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先在地图上点击添加至少 1 个航点！");
        return;
    }
    const double targetSpeed = m_speedSpin->value();
    for (auto &wp : wps) {
        wp.speedMps = targetSpeed;
    }

    logMessage(QString("正在发布 %1 个航点至地面站...").arg(wps.size()));
    m_client->submitWaypoints(wps, 300);
}

void AppWindow::onEmergencyStopClicked() {
    if (QMessageBox::question(this, "警告", "确认紧急制动并停止当前所有任务？") == QMessageBox::Yes) {
        logMessage("正在发送急停命令...");
        m_client->submitEmergencyStop();
    }
}

void AppWindow::onClearWaypointsClicked() {
    m_bridge->clearWaypoints();
    logMessage("航线已清空。");
}

void AppWindow::onUndoWaypointClicked() {
    m_bridge->removeLastWaypoint();
    logMessage("撤销最后一个航点。");
}

void AppWindow::onMissionResult(bool success, const QString &message) {
    logMessage(QString("任务应答: [%1] %2").arg(success ? "成功" : "失败", message));
    if (!success) {
        QMessageBox::critical(this, "错误", message);
    }
}

} // namespace jlusv

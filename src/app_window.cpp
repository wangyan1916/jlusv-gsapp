#include "app_window.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QQmlContext>
#include <QDateTime>
#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QMenu>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTextDocument>
#include <cmath>

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
    connect(m_client, &GsClient::telemetryUpdated, m_bridge, &MapBridge::updateTelemetry);
    connect(m_client, &GsClient::telemetryUpdated, this, &AppWindow::onTelemetryUpdated);
    connect(m_client, &GsClient::groundStationUpdated, m_bridge, &MapBridge::updateGroundStation);
    connect(m_bridge, &MapBridge::relativePositionChanged, this, &AppWindow::refreshPositionDetails);
    connect(m_client, &GsClient::missionProgress, this, &AppWindow::logMessage);
    connect(m_client, &GsClient::missionResult, this, &AppWindow::onMissionResult);
    connect(m_client, &GsClient::busyChanged, this, [this]() { refreshControls(); });
    connect(m_client, &GsClient::fleetUpdated, this, [this](const QJsonArray &members) {
        const QSignalBlocker blocker(m_targetSelect);
        const QString uid = m_client->latestTelemetry().uid;
        m_targetSelect->clear();
        for (const auto &value : members) {
            const auto member = value.toObject();
            const QString id = member.value("uid").toString();
            m_targetSelect->addItem(member.value("name").toString(id) + " · " + id, id);
        }
        int index = m_targetSelect->findData(uid);
        if (index < 0 && !uid.isEmpty()) {
            m_targetSelect->addItem(uid + "（当前无状态）", uid);
            index = m_targetSelect->count() - 1;
        }
        m_targetSelect->setCurrentIndex(index);
        refreshControls();
    });
    connect(m_client, &GsClient::operationUpdated, this, [this](const QJsonObject &op) {
        if (op.isEmpty()) { m_executionLabel->setText("暂无操作回执"); return; }
        const QString state = op.value("execution_fresh").toBool()
            ? executionText(op.value("execution_state").toString()) : "执行状态未确认 / 已过期";
        QString progress;
        if (op.value("waypoint_count").toInt() > 0)
            progress = QString("\n%1进度：%2 / %3").arg(op.value("kind").toString()=="turning"||op.value("kind").toString()=="zigzag"?"实验":"航点")
                .arg(op.value("waypoint_index").toInt()).arg(op.value("waypoint_count").toInt());
        m_executionLabel->setText(QString("目标：%1\n操作：%2\n命令：%3 · 收件：%4\n执行：%5\n执行原因：%6%7\n%8\n收件原因：")
            .arg(op.value("uid").toString(), op.value("operation_id").toString(),
                 op.contains("command_id") ? QString::number(op.value("command_id").toDouble(), 'f', 0) : "待确认",
                 op.value("stage").toString("待确认"), state,
                 executionReasonText(op.value("execution_reason").toString()), progress, op.value("message").toString())
            + op.value("reason").toString("无"));
    });
    connect(m_client, &GsClient::authStatusChanged, this, [this](bool ok, const QString &msg) {
        if (ok) {
            m_statusLabel->setText("状态: 已连接 (安全认证成功)");
            m_statusLabel->setStyleSheet("color: green; font-weight: bold;");
            logMessage("地面站安全认证通过 (已获取写入控制会话)");
        } else {
            m_statusLabel->setText("状态: " + msg);
            m_statusLabel->setStyleSheet("color: #E65100; font-weight: bold;");
            logMessage("地面站认证状态: " + msg);
        }
        refreshControls();
    });

    connect(m_bridge, &MapBridge::waypointsChanged, this, [this]() {
        m_waypointCountLabel->setText(QString("航点数: %1 / 16").arg(m_bridge->rawWaypoints().size()));
        refreshControls();
    });
    refreshControls();
}

void AppWindow::setupUi() {
    setWindowTitle("JLUSV 地面站 " + QCoreApplication::applicationVersion());
    resize(1360, 960);

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
    panelWidget->setMinimumWidth(350);
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
    m_targetSelect = new QComboBox(connGroup);
    connLayout->addWidget(new QLabel("网关地址:"), 0, 0);
    connLayout->addWidget(m_ipEdit, 0, 1, 1, 2);
    connLayout->addWidget(new QLabel("控制密码:"), 1, 0);
    connLayout->addWidget(m_pwdEdit, 1, 1);
    connLayout->addWidget(m_connectBtn, 1, 2);
    connLayout->addWidget(new QLabel("目标船只:"), 2, 0);
    connLayout->addWidget(m_targetSelect, 2, 1, 1, 2);
    connLayout->addWidget(m_statusLabel, 3, 0, 1, 3);
    panelLayout->addWidget(connGroup);

    connect(m_connectBtn, &QPushButton::clicked, this, &AppWindow::onConnectClicked);
    connect(m_targetSelect, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        m_client->setTargetUid(m_targetSelect->itemData(index).toString());
    });

    // 船只状态卡片
    auto *statusGroup = new QGroupBox("船端遥测数据 · GS 相对米制", panelWidget);
    auto *statusLayout = new QGridLayout(statusGroup);
    m_uidLabel = new QLabel("--", statusGroup);
    m_modeLabel = new QLabel("--", statusGroup);
    m_rtkLabel = new QLabel("--", statusGroup);
    m_posLabel = new QLabel("--", statusGroup);
    m_positionSourceLabel = new QLabel("--", statusGroup);
    m_positionSourceLabel->setWordWrap(true);
    m_diffAgeLabel = new QLabel("--", statusGroup);
    m_headingLabel = new QLabel("--", statusGroup);
    m_headingLabel->setWordWrap(true);
    m_cogLabel = new QLabel("--", statusGroup);
    m_speedLabel = new QLabel("--", statusGroup);
    m_batteryLabel = new QLabel("--", statusGroup);

    statusLayout->addWidget(new QLabel("船只 UID:"), 0, 0);
    statusLayout->addWidget(m_uidLabel, 0, 1);
    statusLayout->addWidget(new QLabel("控制模式:"), 1, 0);
    statusLayout->addWidget(m_modeLabel, 1, 1);
    statusLayout->addWidget(new QLabel("接收机解状态:"), 2, 0);
    m_rtkLabel->setToolTip("接口状态：1=非RTK，2=FLOAT，3=FIXED。解状态与当前使用的位置来源分别显示。");
    statusLayout->addWidget(m_rtkLabel, 2, 1);
    statusLayout->addWidget(new QLabel("相对 GS 东 / 北:"), 3, 0);
    statusLayout->addWidget(m_posLabel, 3, 1);
    statusLayout->addWidget(new QLabel("位置来源:"), 4, 0);
    statusLayout->addWidget(m_positionSourceLabel, 4, 1);
    statusLayout->addWidget(new QLabel("接收机差分龄期:"), 5, 0);
    statusLayout->addWidget(m_diffAgeLabel, 5, 1);
    statusLayout->addWidget(new QLabel("艏向:"), 6, 0);
    statusLayout->addWidget(m_headingLabel, 6, 1);
    statusLayout->addWidget(new QLabel("对地航迹向 COG:"), 7, 0);
    statusLayout->addWidget(m_cogLabel, 7, 1);
    statusLayout->addWidget(new QLabel("对地航速:"), 8, 0);
    statusLayout->addWidget(m_speedLabel, 8, 1);
    statusLayout->addWidget(new QLabel("电池电压:"), 9, 0);
    statusLayout->addWidget(m_batteryLabel, 9, 1);
    panelLayout->addWidget(statusGroup);

    auto *positionGroup = new QGroupBox("GS 原点与相对位置", panelWidget);
    auto *positionLayout = new QGridLayout(positionGroup);
    m_gsPosLabel = new QLabel("等待 GS 坐标", positionGroup);
    m_gsPosLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_gsPosLabel->setWordWrap(true);
    m_gsSourceLabel = new QLabel("--", positionGroup);
    m_gsSourceLabel->setWordWrap(true);
    m_gsAgeLabel = new QLabel("--", positionGroup);
    m_relativeDistanceLabel = new QLabel("--", positionGroup);
    m_relativeOffsetLabel = new QLabel("--", positionGroup);
    m_relativeOffsetLabel->setToolTip("以 GS 为原点：东 X 为正、西为负；北 Y 为正、南为负。单位：米。");
    positionLayout->addWidget(new QLabel("GS 纬度 / 经度:"), 0, 0);
    positionLayout->addWidget(m_gsPosLabel, 0, 1);
    positionLayout->addWidget(new QLabel("GS 坐标来源:"), 1, 0);
    positionLayout->addWidget(m_gsSourceLabel, 1, 1);
    positionLayout->addWidget(new QLabel("接收机参考源龄:"), 2, 0);
    positionLayout->addWidget(m_gsAgeLabel, 2, 1);
    positionLayout->addWidget(new QLabel("距 GS / 真北方位:"), 3, 0);
    positionLayout->addWidget(m_relativeDistanceLabel, 3, 1);
    positionLayout->addWidget(new QLabel("东 X / 北 Y:"), 4, 0);
    positionLayout->addWidget(m_relativeOffsetLabel, 4, 1);
    panelLayout->addWidget(positionGroup);

    // 地图与坐标系设置
    auto *mapSettingGroup = new QGroupBox("底图纠偏设置", panelWidget);
    auto *mapSettingLayout = new QVBoxLayout(mapSettingGroup);
    m_gcj02Check = new QCheckBox("底图使用 GCJ-02（WGS-84 底图请取消）", mapSettingGroup);
    m_gcj02Check->setToolTip("按实际瓦片坐标系选择。仅转换地图显示与地图点击坐标；任务下发始终使用 WGS-84。");
    m_gcj02Check->setChecked(m_bridge->useGcj02());
    connect(m_gcj02Check, &QCheckBox::toggled, this, &AppWindow::onGcj02Toggled);
    mapSettingLayout->addWidget(m_gcj02Check);
    panelLayout->addWidget(mapSettingGroup);

    // 航点任务卡片
    auto *missionGroup = new QGroupBox("模式与任务", panelWidget);
    auto *missionLayout = new QGridLayout(missionGroup);
    m_ownerSelect = new QComboBox(missionGroup);
    m_ownerSelect->addItem("主控 · 船端 RP 执行", 1);
    m_ownerSelect->addItem("工控 · NUC 计算执行", 2);
    m_taskSelect = new QComboBox(missionGroup);
    m_taskSelect->addItem("航路点循迹", "waypoints");
    m_taskSelect->addItem("航向＋航速保持", "ipc_heading_speed");
    m_taskSelect->addItem("航向保持（固定推进）", "heading_only");
    m_taskSelect->addItem("舵桨直控", "actuator");
    m_taskSelect->addItem("回转实验", "turning");
    m_taskSelect->addItem("Z 型实验", "zigzag");
    m_taskSelect->addItem("直线定推力实验", "id_1");
    m_taskSelect->addItem("纵向自由衰减实验", "id_2");
    m_taskSelect->addItem("横漂自由衰减实验", "id_3");
    m_taskSelect->addItem("横摇衰减记录（人工扰动）", "id_4");
    m_taskSelect->addItem("台架阶梯推进（外部测力）", "id_5");
    m_applyModeBtn = new QPushButton("应用所选模式", missionGroup);
    m_modeHint = new QLabel(missionGroup);
    m_modeHint->setWordWrap(true);
    m_headingSpin = new QDoubleSpinBox(missionGroup);
    m_headingSpin->setRange(0, 359.99);
    m_headingSpin->setSuffix(" °");
    m_throttleSpin = new QDoubleSpinBox(missionGroup);
    m_throttleSpin->setRange(0, 100);
    m_throttleSpin->setSuffix(" %");
    m_ttlSpin = new QSpinBox(missionGroup);
    m_ttlSpin->setRange(1, 3600);
    m_ttlSpin->setValue(300);
    m_ttlSpin->setSuffix(" s");
    m_uniformSpeed = new QCheckBox("发布时统一替换航点速度", missionGroup);
    m_uniformSpeed->setChecked(false);
    m_waypointCountLabel = new QLabel("航点数: 0 / 16 (点击地图添加)", missionGroup);
    m_speedSpin = new QDoubleSpinBox(missionGroup);
    m_speedSpin->setRange(0.0, 3.0);
    m_speedSpin->setSingleStep(0.1);
    m_speedSpin->setValue(1.2);
    m_speedSpin->setSuffix(" m/s");

    m_publishBtn = new QPushButton("发布循迹任务", missionGroup);
    m_publishBtn->setStyleSheet("background-color: #2E7D32; color: white; font-weight: bold; height: 32px;");
    m_stopBtn = new QPushButton("紧急制动 / 停船", missionGroup);
    m_stopBtn->setStyleSheet("background-color: #C62828; color: white; font-weight: bold; height: 32px;");
    m_undoBtn = new QPushButton("撤销航点", missionGroup);
    m_clearBtn = new QPushButton("清空航线", missionGroup);

    m_copyBtn = new QToolButton(missionGroup);
    m_copyBtn->setText("复制航点");
    m_copyBtn->setPopupMode(QToolButton::MenuButtonPopup);
    m_copyBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto *copyMenu = new QMenu(m_copyBtn);
    copyMenu->addAction("复制为 标准文本 (每行: 纬度, 经度, 速度 m/s) [默认]", this, &AppWindow::onCopyWaypointsText);
    copyMenu->addAction("复制为 JSON 格式", this, &AppWindow::onCopyWaypointsJson);
    m_copyBtn->setMenu(copyMenu);

    m_pasteBtn = new QPushButton("粘贴/导入航点", missionGroup);

    missionLayout->addWidget(new QLabel("执行方:"), 0, 0);
    missionLayout->addWidget(m_ownerSelect, 0, 1);
    missionLayout->addWidget(new QLabel("任务类型:"), 1, 0);
    missionLayout->addWidget(m_taskSelect, 1, 1);
    missionLayout->addWidget(m_applyModeBtn, 2, 0, 1, 2);
    missionLayout->addWidget(m_modeHint, 3, 0, 1, 2);
    missionLayout->addWidget(new QLabel("目标控制方向:"), 4, 0);
    missionLayout->addWidget(m_headingSpin, 4, 1);
    missionLayout->addWidget(new QLabel("航速 / 新增航点默认速度:"), 5, 0);
    missionLayout->addWidget(m_speedSpin, 5, 1);
    missionLayout->addWidget(new QLabel("固定推进（0%仍控舵）:"), 6, 0);
    missionLayout->addWidget(m_throttleSpin, 6, 1);
    missionLayout->addWidget(new QLabel("航点任务有效期:"), 7, 0);
    missionLayout->addWidget(m_ttlSpin, 7, 1);
    missionLayout->addWidget(m_uniformSpeed, 8, 0, 1, 2);
    missionLayout->addWidget(m_waypointCountLabel, 9, 0, 1, 2);
    missionLayout->addWidget(m_undoBtn, 10, 0);
    missionLayout->addWidget(m_clearBtn, 10, 1);
    missionLayout->addWidget(m_copyBtn, 11, 0);
    missionLayout->addWidget(m_pasteBtn, 11, 1);
    missionLayout->addWidget(m_publishBtn, 12, 0, 1, 2);
    missionLayout->addWidget(m_stopBtn, 13, 0, 1, 2);
    m_maneuverGroup = new QGroupBox("操纵实验参数", missionGroup);
    auto *experimentLayout = new QGridLayout(m_maneuverGroup);
    m_maneuverRudder = new QDoubleSpinBox(m_maneuverGroup);
    m_maneuverRudder->setRange(-30,30); m_maneuverRudder->setDecimals(1); m_maneuverRudder->setValue(10); m_maneuverRudder->setSuffix(" °");
    m_maneuverRudder->setToolTip("正值先右转，负值先左转；相对舵零，0°不能作为实验激励。");
    m_maneuverThreshold = new QDoubleSpinBox(m_maneuverGroup);
    m_maneuverThreshold->setRange(1,90); m_maneuverThreshold->setDecimals(1); m_maneuverThreshold->setValue(10); m_maneuverThreshold->setSuffix(" °");
    m_maneuverDriveMode = new QComboBox(m_maneuverGroup);
    m_maneuverDriveMode->addItem("固定推进",0); m_maneuverDriveMode->addItem("保持航速",1);
    m_maneuverDrive = new QDoubleSpinBox(m_maneuverGroup);
    m_maneuverDrive->setRange(0,100); m_maneuverDrive->setValue(10); m_maneuverDrive->setSuffix(" %");
    m_maneuverCount = new QSpinBox(m_maneuverGroup);
    m_maneuverCount->setRange(4,16); m_maneuverCount->setValue(6); m_maneuverCount->setSuffix(" ×90°");
    m_maneuverCountLabel = new QLabel("回转总角度:",m_maneuverGroup);
    m_maneuverSeconds = new QSpinBox(m_maneuverGroup);
    m_maneuverSeconds->setRange(10,600); m_maneuverSeconds->setValue(120); m_maneuverSeconds->setSuffix(" s");
    experimentLayout->addWidget(new QLabel("舵角 / 起始方向:"),0,0); experimentLayout->addWidget(m_maneuverRudder,0,1);
    experimentLayout->addWidget(new QLabel("推进方式:"),1,0); experimentLayout->addWidget(m_maneuverDriveMode,1,1);
    experimentLayout->addWidget(new QLabel("推进 / 航速目标:"),2,0); experimentLayout->addWidget(m_maneuverDrive,2,1);
    experimentLayout->addWidget(new QLabel("Z 型反舵偏角:"),3,0); experimentLayout->addWidget(m_maneuverThreshold,3,1);
    experimentLayout->addWidget(m_maneuverCountLabel,4,0); experimentLayout->addWidget(m_maneuverCount,4,1);
    experimentLayout->addWidget(new QLabel("最长总时长:"),5,0); experimentLayout->addWidget(m_maneuverSeconds,5,1);
    m_settleSeconds=new QSpinBox(m_maneuverGroup);m_settleSeconds->setRange(1,60);m_settleSeconds->setValue(5);m_settleSeconds->setSuffix(" s");
    m_holdSeconds=new QSpinBox(m_maneuverGroup);m_holdSeconds->setRange(1,300);m_holdSeconds->setValue(20);m_holdSeconds->setSuffix(" s / 档");
    m_coastSeconds=new QSpinBox(m_maneuverGroup);m_coastSeconds->setRange(0,300);m_coastSeconds->setValue(20);m_coastSeconds->setSuffix(" s");
    experimentLayout->addWidget(new QLabel("准备时间:"),6,0);experimentLayout->addWidget(m_settleSeconds,6,1);
    experimentLayout->addWidget(new QLabel("保持 / 记录时间:"),7,0);experimentLayout->addWidget(m_holdSeconds,7,1);
    experimentLayout->addWidget(new QLabel("断推滑行时间:"),8,0);experimentLayout->addWidget(m_coastSeconds,8,1);
    auto *experimentHint = new QLabel("回转/Z型先准备5秒；直线/衰减使用下方时长。横漂须确认机构可到±90°；横摇仅记录、人工扰动；台架须先固定并配外部测力。原始记录保存在IPC，停止按钮随时取消。",m_maneuverGroup);
    experimentHint->setWordWrap(true); experimentLayout->addWidget(experimentHint,9,0,1,2);
    missionLayout->addWidget(m_maneuverGroup,14,0,1,2); m_maneuverGroup->hide();
    connect(m_maneuverDriveMode,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){
        const bool speed=m_maneuverDriveMode->currentData().toInt()==1;
        m_maneuverDrive->setRange(speed?.1:0,speed?3:100); m_maneuverDrive->setValue(speed?1:10);
        m_maneuverDrive->setSuffix(speed?" m/s":" %");
    });
    connect(m_taskSelect,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int){
        const QString kind=m_taskSelect->currentData().toString(); const bool zig=kind=="zigzag";
        const bool identification=kind.startsWith("id_");const int profile=identification?kind.mid(3).toInt():0;
        if(kind=="turning"||zig||identification)m_ownerSelect->setCurrentIndex(m_ownerSelect->findData(2));
        m_maneuverCount->setRange(identification||zig?1:4,16);m_maneuverCount->setValue(identification?(profile==5?5:1):zig?4:6);
        m_maneuverCount->setSuffix(identification?" 档":zig?" 次":" ×90°");m_maneuverCountLabel->setText(identification?"推进档位数:":zig?"反舵次数:":"回转总角度:");
        m_maneuverRudder->setRange(profile==3?-90:-30,profile==3?90:30);m_maneuverRudder->setValue(profile==3?90:identification?0:10);
        if(identification){m_maneuverDriveMode->setCurrentIndex(0);m_maneuverDrive->setValue(profile==4?0:10);m_maneuverSeconds->setValue(profile==5?180:120);}
        refreshControls();
    });
    panelLayout->addWidget(missionGroup);
    auto *feedbackGroup = new QGroupBox("船端执行反馈", panelWidget);
    auto *feedbackLayout = new QVBoxLayout(feedbackGroup);
    m_executionLabel = new QLabel("暂无操作回执", feedbackGroup);
    m_executionLabel->setWordWrap(true);
    m_executionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    feedbackLayout->addWidget(m_executionLabel);
    auto *queryBtn = new QPushButton("刷新操作回执", feedbackGroup);
    feedbackLayout->addWidget(queryBtn);
    panelLayout->addWidget(feedbackGroup);
    connect(queryBtn, &QPushButton::clicked, m_client, &GsClient::queryOperation);
    connect(m_ownerSelect, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { refreshControls(); });
    connect(m_taskSelect, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { refreshControls(); });
    connect(m_applyModeBtn, &QPushButton::clicked, this, [this]() {
        m_client->switchControlMode(m_ownerSelect->currentData().toInt(), selectedWork());
    });
    connect(m_speedSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), m_bridge, &MapBridge::setDefaultSpeed);

    connect(m_publishBtn, &QPushButton::clicked, this, &AppWindow::onPublishWaypointsClicked);
    connect(m_stopBtn, &QPushButton::clicked, this, &AppWindow::onEmergencyStopClicked);
    connect(m_clearBtn, &QPushButton::clicked, this, &AppWindow::onClearWaypointsClicked);
    connect(m_undoBtn, &QPushButton::clicked, this, &AppWindow::onUndoWaypointClicked);
    connect(m_copyBtn, &QToolButton::clicked, this, &AppWindow::onCopyWaypointsText);
    connect(m_pasteBtn, &QPushButton::clicked, this, &AppWindow::onPasteWaypointsClicked);

    // 系统日志框
    auto *logGroup = new QGroupBox("操作日志", panelWidget);
    auto *logLayout = new QVBoxLayout(logGroup);
    m_logEdit = new QTextEdit(logGroup);
    m_logEdit->setReadOnly(true);
    m_logEdit->setMinimumHeight(130);
    m_logEdit->document()->setMaximumBlockCount(500);
    logLayout->addWidget(m_logEdit);
    panelLayout->addWidget(logGroup, 1);

    auto *panelScroll = new QScrollArea(this);
    panelScroll->setWidgetResizable(true);
    panelScroll->setMinimumWidth(400);
    panelScroll->setWidget(panelWidget);
    mainLayout->addWidget(panelScroll, 1);

    logMessage("地面站启动完成。请配置网关地址并连接。");
}

void AppWindow::logMessage(const QString &msg) {
    const QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_logEdit->append(QString("[%1] %2").arg(timestamp, msg));
}

void AppWindow::onConnectClicked() {
    m_bridge->useAutomaticGroundStation();
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
    refreshControls();
}

void AppWindow::refreshPositionDetails() {
    const auto &p = m_bridge->groundStationPosition();
    const auto anchor=m_bridge->groundStationAnchor();
    m_gsPosLabel->setText(m_bridge->manualGroundStation() || p.available
        ? QString("%1, %2%3").arg(anchor.latitude(), 0, 'f', 7).arg(anchor.longitude(), 0, 'f', 7)
            .arg(m_bridge->manualGroundStation() ? "（地图手点）" : p.old ? "（旧值）" : "") : "无可用 GS 坐标");
    QString source = p.source;
    if (source == "RTCM1005") source = "RTCM 1005 基站坐标";
    else if (source == "NMEA_GGA") source = "NMEA GGA";
    else if (source == "SVIN_ESTIMATE") source = "自定位暂估";
    else if (source == "SVIN_RESULT") source = "自定位结果";
    else if (source == "FIXED_REFERENCE") source = "固定参考坐标";
    else if (source.isEmpty() || source == "UNKNOWN") source = "未知";
    m_gsSourceLabel->setText((m_bridge->manualGroundStation() ? "地图校正 · 原始参考：" : "自动 · ")
        + source + " · " + (!p.valid ? "无效 / 已过期" : p.old ? "旧参考" : p.estimated ? "暂估" : "有效"));
    m_gsAgeLabel->setText(p.ageMs >= 0 ? QString("%1 s").arg(p.ageMs / 1000.0, 0, 'f', 2) : "未知");
    const auto &r = m_bridge->relativePosition();
    m_posLabel->setText(r.valid
        ? QString("%1 m / %2 m").arg(r.eastM,0,'f',2).arg(r.northM,0,'f',2) : "无有效相对位置");
    m_relativeDistanceLabel->setText(r.valid
        ? QString("%1 m / %2°").arg(r.distanceM, 0, 'f', 2).arg(r.bearingDeg, 0, 'f', 1) : "—");
    m_relativeOffsetLabel->setText(r.valid
        ? QString("%1 m / %2 m").arg(r.eastM, 0, 'f', 2).arg(r.northM, 0, 'f', 2) : "—");
}

void AppWindow::onTelemetryUpdated(const UsvTelemetry &t) {
    m_uidLabel->setText(t.uid.isEmpty() ? "--" : t.uid);

    if (!t.controlFresh) {
        m_modeLabel->setText("未知 / 回报过期");
        m_modeLabel->setStyleSheet("color: gray;");
    } else if (t.controlOwner == 1 && t.controlState == 3) {
        m_modeLabel->setText("MAIN / WAYPOINTS (循迹)");
        m_modeLabel->setStyleSheet("color: green; font-weight: bold;");
    } else if (t.controlOwner == 1 && t.controlState == 1) {
        m_modeLabel->setText("MAIN / ACTUATOR (直控)");
        m_modeLabel->setStyleSheet("color: #E65100; font-weight: bold;");
    } else if (t.controlOwner == 1 && t.controlState == 2) {
        m_modeLabel->setText("MAIN / HEADING (定向)");
        m_modeLabel->setStyleSheet("color: #1565C0; font-weight: bold;");
    } else if (t.controlOwner == 2) {
        m_modeLabel->setText("IPC / " + QString(t.controlState == 3 ? "航路点循迹" : t.controlState == 2 ? "航向控制" : "舵桨直控"));
        m_modeLabel->setStyleSheet("color: #6A1B9A; font-weight: bold;");
    } else {
        m_modeLabel->setText(t.controlOwnerStr + " / " + t.controlStateStr);
        m_modeLabel->setStyleSheet("color: gray;");
    }

    m_rtkLabel->setText(m_bridge->rtkStateText());
    m_posLabel->setToolTip(t.positionValid ? QString("原始 WGS-84：%1, %2").arg(t.latDeg,0,'f',7).arg(t.lonDeg,0,'f',7) : "无有效船位");
    m_positionSourceLabel->setText(m_bridge->positionSourceText());
    m_diffAgeLabel->setText(t.diffAgeMs >= 0 ? QString("%1 s").arg(t.diffAgeMs / 1000.0, 0, 'f', 2)
        : t.diffAgeState == "STALE" || t.diffAgeState == "EXPIRED" ? "过期" : "未知 / 未提供");
    m_headingLabel->setText((t.headingValid ? QString("%1°").arg(t.headingDeg, 0, 'f', 1) : "无有效艏向")
        + "\n" + m_bridge->headingSourceText());
    m_headingLabel->setToolTip(t.headingReason);
    m_cogLabel->setText(t.cogValid ? QString("%1°（真北）").arg(t.cogDeg, 0, 'f', 1) : "无有效 COG");
    m_speedLabel->setText(t.speedValid ? QString("%1 m/s").arg(t.sogMps, 0, 'f', 2) : "无有效航速");
    m_batteryLabel->setText(t.batteryValid ? QString("%1 V").arg(t.batteryV, 0, 'f', 2) : "--");
    refreshControls();
}

int AppWindow::selectedWork() const {
    const QString kind = m_taskSelect->currentData().toString();
    return kind == "actuator" ? 1 : kind == "waypoints" ? 3 : 2;
}

void AppWindow::refreshControls() {
    const bool busy = m_client->isBusy();
    const int owner = m_ownerSelect->currentData().toInt(), work = selectedWork();
    const QString kind = m_taskSelect->currentData().toString();
    const bool identification=kind.startsWith("id_");const int profile=identification?kind.mid(3).toInt():0;
    const bool experiment=kind=="turning"||kind=="zigzag"||identification;
    m_maneuverGroup->setVisible(experiment); m_maneuverGroup->setEnabled(!busy);
    m_maneuverThreshold->setEnabled(kind=="zigzag");
    m_maneuverDriveMode->setEnabled(!identification);m_maneuverDrive->setEnabled(profile!=4);
    m_maneuverRudder->setEnabled(!identification||profile==3);m_maneuverCount->setEnabled(!identification||profile==5);
    m_settleSeconds->setEnabled(identification);m_holdSeconds->setEnabled(identification);
    m_coastSeconds->setEnabled(profile==2||profile==3);
    const bool selected = m_client->canPublish(owner, work);
    m_targetSelect->setEnabled(!busy);
    m_ownerSelect->setEnabled(!busy&&!experiment);
    m_taskSelect->setEnabled(!busy);
    m_connectBtn->setEnabled(!busy);
    m_applyModeBtn->setEnabled(!busy && m_client->canControl());
    m_publishBtn->setEnabled(selected && work != 1 && (work != 3 || !m_bridge->rawWaypoints().isEmpty()));
    m_publishBtn->setText("发布" + m_ownerSelect->currentText().section(" · ", 0, 0) + m_taskSelect->currentText());
    m_stopBtn->setEnabled(m_client->canControl(false));
    m_headingSpin->setEnabled(!busy && work == 2 && !experiment);
    m_speedSpin->setEnabled(!busy && kind != "heading_only" && work != 1 && !experiment);
    m_throttleSpin->setEnabled(!busy && kind == "heading_only");
    m_ttlSpin->setEnabled(!busy && work == 3);
    m_uniformSpeed->setEnabled(!busy && work == 3);
    if (busy) m_modeHint->setText("正在等待操作确认；停船按钮仍可抢占当前操作。");
    else if (!m_client->canControl()) m_modeHint->setText("请连接认证并等待目标船LoRa身份与新鲜控制模式回报。");
    else if (!selected) m_modeHint->setText("所选模式与船端不同。请应用模式，等待实际生效后发布。");
    else if (work == 1) m_modeHint->setText(owner == 1
        ? "主控舵桨模式已确认。操纵请使用网关遥控页 /remote。" : "工控舵桨模式已确认，等待对应的新ROS舵桨主题。");
    else if(experiment)m_modeHint->setText("工控任务模式已确认。实验由 IPC 本地执行；请先停船，再发布实验。停止按钮可随时取消。");
    else m_modeHint->setText(kind == "heading_only"
        ? "模式已确认。固定推进只需有效控制方向；0%仍会自动控舵。"
        : work == 3 ? "模式已确认。航点使用WGS-84；默认保留逐点速度。" : "模式已确认。发布后等待有效控制方向与航速反馈。");
}

void AppWindow::onGcj02Toggled(bool checked) {
    m_bridge->setUseGcj02(checked);
    logMessage(QString("底图坐标逆纠偏模式: %1").arg(checked ? "启用 (GCJ-02)" : "禁用 (真实 WGS-84)"));
}

void AppWindow::onPublishWaypointsClicked() {
    const int owner = m_ownerSelect->currentData().toInt();
    const QString kind = m_taskSelect->currentData().toString();
    if(kind.startsWith("id_")){
        const int profile=kind.mid(3).toInt();
        m_client->submitIdentification(profile,profile==4?0:m_maneuverDrive->value(),profile==3?m_maneuverRudder->value():0,
            profile==5?m_maneuverCount->value():1,m_settleSeconds->value(),m_holdSeconds->value(),
            profile==2||profile==3?m_coastSeconds->value():0,m_maneuverSeconds->value());return;
    }
    if(kind=="turning"||kind=="zigzag"){
        m_client->submitManeuver(kind=="zigzag",m_maneuverRudder->value(),m_maneuverDriveMode->currentData().toInt()==1,
            m_maneuverDrive->value(),kind=="zigzag"?m_maneuverThreshold->value():0,m_maneuverCount->value(),m_maneuverSeconds->value());
        return;
    }
    if (kind == "ipc_heading_speed") {
        m_client->submitHeadingSpeed(owner, m_headingSpin->value(), m_speedSpin->value());
        return;
    }
    if (kind == "heading_only") {
        m_client->submitHeadingOnly(owner, m_headingSpin->value(), m_throttleSpin->value());
        return;
    }
    if (kind != "waypoints") return;
    auto wps = m_bridge->rawWaypoints();
    if (wps.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先在地图上点击添加至少 1 个航点！");
        return;
    }
    const double targetSpeed = m_speedSpin->value();
    if (m_uniformSpeed->isChecked()) for (auto &wp : wps) wp.speedMps = targetSpeed;

    logMessage(QString("正在发布 %1 个航点至地面站...").arg(wps.size()));
    m_client->submitWaypoints(wps, m_ttlSpin->value(), owner);
}

void AppWindow::onEmergencyStopClicked() {
    logMessage("正在发送急停命令，等待船端输出关闭证明...");
    m_client->submitEmergencyStop();
}

void AppWindow::onClearWaypointsClicked() {
    m_bridge->clearWaypoints();
    logMessage("航线已清空。");
}

void AppWindow::onUndoWaypointClicked() {
    m_bridge->removeLastWaypoint();
    logMessage("撤销最后一个航点。");
}

void AppWindow::onCopyWaypointsJson() {
    const auto &wps = m_bridge->rawWaypoints();
    if (wps.isEmpty()) {
        QMessageBox::information(this, "提示", "当前航点列表为空，请先在地图上点击添加航点！");
        return;
    }

    QJsonArray arr;
    for (int i = 0; i < wps.size(); ++i) {
        QJsonObject obj;
        obj.insert("index", i + 1);
        obj.insert("lat_deg", QString::number(wps[i].latDeg, 'f', 7).toDouble());
        obj.insert("lon_deg", QString::number(wps[i].lonDeg, 'f', 7).toDouble());
        obj.insert("speed_mps", QString::number(wps[i].speedMps, 'f', 2).toDouble());
        arr.append(obj);
    }

    const QString jsonText = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    QApplication::clipboard()->setText(jsonText);
    logMessage(QString("已将 %1 个航点坐标复制到剪贴板 (JSON 格式)。").arg(wps.size()));
    for (int i = 0; i < wps.size(); ++i) {
        logMessage(QString("  #%1: 纬度 %2, 经度 %3 (航速 %4 m/s)")
            .arg(i + 1)
            .arg(wps[i].latDeg, 0, 'f', 7)
            .arg(wps[i].lonDeg, 0, 'f', 7)
            .arg(wps[i].speedMps, 0, 'f', 2));
    }
}

void AppWindow::onCopyWaypointsText() {
    const auto &wps = m_bridge->rawWaypoints();
    if (wps.isEmpty()) {
        QMessageBox::information(this, "提示", "当前航点列表为空，请先在地图上点击添加航点！");
        return;
    }

    QStringList lines;
    for (int i = 0; i < wps.size(); ++i) {
        lines << QString("%1, %2, %3 m/s")
            .arg(wps[i].latDeg, 0, 'f', 7)
            .arg(wps[i].lonDeg, 0, 'f', 7)
            .arg(wps[i].speedMps, 0, 'f', 2);
    }
    const QString text = lines.join("\n");
    QApplication::clipboard()->setText(text);
    logMessage(QString("已将 %1 个航点坐标复制到剪贴板 (每行: 纬度, 经度, 速度 m/s)。").arg(wps.size()));
    for (const auto &line : lines) {
        logMessage("  " + line);
    }
}

void AppWindow::onPasteWaypointsClicked() {
    const QString text = QApplication::clipboard()->text().trimmed();
    if (text.isEmpty()) {
        QMessageBox::information(this, "提示", "系统剪贴板为空，请先复制航点数据！");
        return;
    }

    QVector<Waypoint> imported;
    const double defaultSpeed = m_speedSpin->value();
    bool invalid = false;

    // 1. 尝试解析 JSON 格式
    QJsonParseError jsonErr;
    QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &jsonErr);
    if (jsonErr.error == QJsonParseError::NoError && doc.isArray()) {
        const QJsonArray arr = doc.array();
        for (const auto &val : arr) {
            if (val.isObject()) {
                const QJsonObject obj = val.toObject();
                Waypoint wp;
                const auto latValue = obj.value(obj.contains("lat") ? "lat" : "lat_deg");
                const auto lonValue = obj.value(obj.contains("lon") ? "lon" : "lon_deg");
                const auto speedValue = obj.value(obj.contains("speed_mps") ? "speed_mps" : "speed");
                if (!latValue.isDouble() || !lonValue.isDouble() ||
                    (!speedValue.isUndefined() && !speedValue.isDouble())) { invalid = true; break; }
                // 支持 lat / lat_deg
                if (obj.contains("lat")) wp.latDeg = obj.value("lat").toDouble();
                else if (obj.contains("lat_deg")) wp.latDeg = obj.value("lat_deg").toDouble();
                else continue;

                // 支持 lon / lon_deg
                if (obj.contains("lon")) wp.lonDeg = obj.value("lon").toDouble();
                else if (obj.contains("lon_deg")) wp.lonDeg = obj.value("lon_deg").toDouble();
                else continue;

                // 支持 speed_mps / speed
                if (obj.contains("speed_mps")) wp.speedMps = obj.value("speed_mps").toDouble();
                else if (obj.contains("speed")) wp.speedMps = obj.value("speed").toDouble();
                else wp.speedMps = defaultSpeed;

                if (std::isfinite(wp.latDeg) && std::isfinite(wp.lonDeg) && std::isfinite(wp.speedMps) &&
                    wp.latDeg >= -90.0 && wp.latDeg <= 90.0 && wp.lonDeg >= -180.0 && wp.lonDeg <= 180.0 &&
                    wp.speedMps >= 0 && wp.speedMps <= 3) {
                    imported.append(wp);
                } else { invalid = true; break; }
            } else { invalid = true; break; }
        }
    }

    // 2. 若非 JSON，按行纯文本解析 (每行: 纬度, 经度, 速度 m/s 或 纬度, 经度)
    if (jsonErr.error != QJsonParseError::NoError && !text.startsWith('[') && !text.startsWith('{')) {
        const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
        for (QString line : lines) {
            line = line.trimmed();
            if (line.isEmpty() || line.startsWith('#') || line.startsWith("//")) continue;
            int colonIdx = line.indexOf(':');
            if (colonIdx >= 0 && colonIdx < 5) {
                line = line.mid(colonIdx + 1).trimmed();
            }
            line.remove("m/s", Qt::CaseInsensitive);
            line.remove("mps", Qt::CaseInsensitive);

            static const QRegularExpression re(R"(^([+-]?\d+\.?\d*)\s*[,，\s]\s*([+-]?\d+\.?\d*)(?:\s*[,，\s]\s*([+-]?\d+\.?\d*))?\s*$)");
            const auto match = re.match(line);
            if (match.hasMatch()) {
                double lat = match.captured(1).toDouble();
                double lon = match.captured(2).toDouble();
                double spd = defaultSpeed;
                if (!match.captured(3).isEmpty()) {
                    spd = match.captured(3).toDouble();
                }
                if (std::isfinite(lat) && std::isfinite(lon) && std::isfinite(spd) &&
                    lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0 && spd >= 0 && spd <= 3) {
                    Waypoint wp;
                    wp.latDeg = lat;
                    wp.lonDeg = lon;
                    wp.speedMps = spd;
                    imported.append(wp);
                } else { invalid = true; break; }
            } else { invalid = true; break; }
        }
    }

    if (invalid || imported.isEmpty() || imported.size() > 16) {
        QMessageBox::warning(this, "解析失败", "航线未改变。请提供1～16个合法WGS-84航点，速度0～3 m/s。\n支持每行：纬度, 经度, 速度 m/s；或JSON数组。无效行不会被跳过，超量不会被截断。");
        return;
    }

    m_bridge->setRawWaypoints(imported);
    logMessage(QString("已成功从剪贴板导入 %1 个航点并更新到地图！").arg(imported.size()));
    QMessageBox::information(this, "导入成功", QString("已成功导入 %1 个航点，航线已在地图上绘制显示！").arg(imported.size()));
}

void AppWindow::onMissionResult(bool success, const QString &message) {
    logMessage(QString("操作反馈: [%1] %2").arg(success ? "已确认" : "未完成 / 未确认", message));
    m_executionLabel->setText(message + "\n" + m_executionLabel->text());
    refreshControls();
}

} // namespace jlusv

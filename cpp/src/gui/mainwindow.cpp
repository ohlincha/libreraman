#include "core/calibration.h"
#include "mainwindow.h"
#include "datapage.h"
#include "datapagehost.h"

#include "apiserver.h"
#include "desktopentry.h"
#include "helpwindow.h"

#include "measurepage.h"
#include "page.h"
#include "statusstrip.h"
#include "theme.h"

#include "core/apiclient.h"
#include "core/errors.h"
#include "core/simulator.h"
#include "core/windrivers.h"

#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <memory>

namespace lr::gui {

namespace {
const char* const kOnCloseKey = "instrument/onClose";
const char* const kApiEnabledKey = "api/enabled";
const char* const kApiPortKey = "api/port";
const char* const kRemoteControlKey = "api/allowRemoteControl";
const QString kTitle = QStringLiteral("LibreRamen " LIBRERAMEN_VERSION);
}  // namespace

MainWindow::MainWindow(Controller* ctl, QWidget* parent) : QMainWindow(parent), m_ctl(ctl) {
    setWindowTitle(kTitle);
    setMinimumSize(1000, 640);
    fitToScreen();
    auto* central = new QWidget;
    auto* outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    m_strip = new StatusStrip;
    connect(m_strip, &StatusStrip::stopClicked, this, [this] {
        addLog(QStringLiteral("GUI: STOP"));
        m_ctl->stop();
    });
    outer->addWidget(m_strip);
    m_simBar = buildSimulatorBar();
    m_simBar->hide();
    outer->addWidget(m_simBar);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    auto* side = new QVBoxLayout;
    side->setContentsMargins(6, 8, 0, 8);
    m_stack = new QStackedWidget;
    m_measure = new MeasurePage(ctl);
    PageContext ctx;
    ctx.controller = ctl;
    ctx.processing = [m = m_measure] { return m->processingSettings(); };
    ctx.window = this;
    const QStringList& areas = pageAreas();
    for (int i = 0; i < areas.size(); ++i) {
        Page* p = i == 0 ? m_measure : createPage(areas[i], ctx);
        m_pages.push_back(p);
        connect(p, &Page::logMessage, this, &MainWindow::addLog);
        auto* b = new QToolButton;
        b->setText(areas[i]);
        b->setCheckable(true);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setFixedSize(104, 44);
        b->setStyleSheet(theme::sideBarButtonStyle());
        connect(b, &QToolButton::clicked, this, [this, i] { showArea(i); });
        side->addWidget(b);
        m_sideButtons.push_back(b);
        m_stack->addWidget(p);
    }
    side->addStretch();
    body->addLayout(side);
    body->addWidget(m_stack, 1);
    outer->addLayout(body, 1);

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    // compact on low screens (1080p at 125 %, laptops): the Measure page needs the height
    const QScreen* scr0 = QGuiApplication::primaryScreen();
    const bool low = scr0 && scr0->availableGeometry().height() < 1000;
    m_log->setMaximumHeight(low ? 46 : 64);
    m_log->setMaximumBlockCount(5000);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(9);
    m_log->setFont(mono);
    outer->addWidget(m_log);
    setCentralWidget(central);

    m_api = new ApiServer(ctl, this);
    m_api->setDataFolder([m = m_measure] { return m->folder(); });
    m_api->setProcessing([m = m_measure] { return m->processingSettings(); });
    m_api->setRemoteControl(QSettings().value(QLatin1String(kRemoteControlKey), false).toBool());
    connect(m_api, &ApiServer::logMessage, this, &MainWindow::addLog);
    connect(m_api, &ApiServer::runningChanged, this, &MainWindow::updateRemoteTile);
    connect(m_api, &ApiServer::remoteControlChanged, this, &MainWindow::updateRemoteTile);

    showArea(0);
    buildMenus();
    updateRemoteTile();

    connect(ctl, &Controller::status, this, &MainWindow::onStatus);
    connect(ctl, &Controller::log, this, &MainWindow::addLog);
    connect(ctl, &Controller::error, this, &MainWindow::onError);
    connect(ctl, &Controller::spectrum, this, &MainWindow::onSpectrum);
    connect(ctl, &Controller::connected, this, &MainWindow::onConnected);
    connect(ctl, &Controller::connectFailed, this, &MainWindow::onConnectFailed);
    connect(m_measure, &MeasurePage::addToLibraryRequested, this, &MainWindow::addToLibrary);
    // the instrument tile's context menu: Reconnect, Shut down
    Tile* inst = m_strip->instrumentTile();
    inst->setContextMenuPolicy(Qt::ActionsContextMenu);
    auto* reconnect = new QAction(QStringLiteral("Reconnect"), inst);
    connect(reconnect, &QAction::triggered, this, [this] { connectInstrument(false); });
    inst->addAction(reconnect);
    inst->addAction(m_shutDownAction);
}

bool MainWindow::addToLibrary(const lr::Spectrum& sp) {
    Page* lib = page(QStringLiteral("Library"));
    if (!lib) {
        addLog(QStringLiteral("Add to library: no Library area"));
        return false;
    }
    const bool ok = lib->addToLibrary(sp);
    if (ok)
        addLog(QStringLiteral("Added %1 to the library").arg(spectrumDisplayName(sp)));
    return ok;
}

bool MainWindow::matchInLibrary(const lr::Spectrum& sp) {
    Page* lib = page(QStringLiteral("Library"));
    if (!lib) {
        addLog(QStringLiteral("Match: no Library area"));
        return false;
    }
    showArea(int(pageAreas().indexOf(QStringLiteral("Library"))));
    return lib->matchSpectrum(sp);
}

void MainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    if (m_offerDesktopIcon) {
        m_offerDesktopIcon = false;
        QTimer::singleShot(1000, this, [this] { desktop::offerOnce(this); });
    }
}

void MainWindow::raiseWindow() {
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
}

void MainWindow::shutDownInstrument() {
    if (m_ctl->connection() != Controller::Connection::Connected || !m_ctl->instrument())
        return;
    const bool sim = m_ctl->simulated();
    if (dialogsEnabled()) {
        const QString text =
            m_ctl->isBusy()
                ? QStringLiteral("<b>A measurement is running.</b><p>Stop it and switch the Raman spectrometer off?</p>")
                : QStringLiteral("<b>Switch the Raman spectrometer off?</b>");
        QMessageBox q(this);
        q.setWindowTitle(QStringLiteral("Shut down instrument"));
        q.setIcon(QMessageBox::Question);
        q.setText(text);
        q.setInformativeText(sim ? QStringLiteral("The simulator is disconnected.")
                                 : QStringLiteral("The laser goes off and the instrument switches off (as its Off "
                                                  "button). To measure again it must be switched on and initialised "
                                                  "(about a minute)."));
        QPushButton* down = q.addButton(QStringLiteral("Shut down"), QMessageBox::AcceptRole);
        q.addButton(QMessageBox::Cancel);
        q.setDefaultButton(QMessageBox::Cancel);
        q.exec();
        if (q.clickedButton() != down)
            return;
    }
    if (!m_ctl->isBusy()) {
        finishShutDown();
        return;
    }
    addLog(QStringLiteral("Stopping the measurement…"));
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_ctl, &Controller::busyChanged, this, [this, conn](bool busy) {
        if (busy)
            return;
        disconnect(*conn);
        finishShutDown();
    });
    m_ctl->stop();
}

void MainWindow::finishShutDown() {
    if (!m_ctl->instrument())
        return;
    const bool sim = m_ctl->simulated();
    m_measure->setReady(false);
    m_info = {};
    m_ctl->disconnectInstrument(!sim);                // real instrument: vendor power-down sequence
    addLog(sim ? QStringLiteral("Instrument shut down (simulator disconnected)") : QStringLiteral("Instrument shut down"));
    if (m_shutDownAction)
        m_shutDownAction->setEnabled(false);
}

QWidget* MainWindow::buildSimulatorBar() {
    auto* bar = new QWidget;
    auto* sl = new QHBoxLayout(bar);
    sl->setContentsMargins(10, 0, 10, 0);
    sl->addWidget(new QLabel(QStringLiteral("<b>Simulator:</b> sample")));
    m_simSample = new QComboBox;
    QStringList names;
    try {
        for (const auto& s : lr::simulatorData().samples)
            names << s.first;
    } catch (const lr::Error&) {
    }
    // Python order (ethanol first: the simulator's default sample)
    QStringList ordered;
    for (const char* n : {"ethanol", "chloroform", "carbon tetrachloride", "water", "zinc sulfide"})
        if (names.contains(QLatin1String(n)))
            ordered << QLatin1String(n);
    for (const QString& n : names)
        if (!ordered.contains(n))
            ordered << n;
    ordered << lr::simulatorReactionNames();       // spectra change with time: for trying time series
    m_simSample->addItems(ordered);
    connect(m_simSample, &QComboBox::currentTextChanged, m_ctl, &Controller::simSetSample);
    sl->addWidget(m_simSample);
    const std::pair<const char*, void (Controller::*)()> buttons[] = {
        {"Press ARM", &Controller::simPressArm},
        {"Open compartment", &Controller::simOpenCompartment},
        {"Close compartment", &Controller::simCloseCompartment}};
    for (const auto& [label, slot] : buttons) {
        auto* b = new QPushButton(QString::fromLatin1(label));
        connect(b, &QPushButton::clicked, m_ctl, slot);
        sl->addWidget(b);
    }
    auto* acc = new QCheckBox(QStringLiteral("Lid-open accessory"));
    acc->setToolTip(QStringLiteral("Simulates the accessory that lets the laser run with the lid open (Focus)"));
    connect(acc, &QCheckBox::toggled, m_ctl, &Controller::simSetLidAccessory);
    sl->addWidget(acc);
    sl->addStretch();
    return bar;
}

void MainWindow::buildMenus() {
    QMenu* m = menuBar()->addMenu(QStringLiteral("&File"));
    QAction* a = m->addAction(QStringLiteral("Quit"), this, &QWidget::close);
    a->setShortcut(QKeySequence::Quit);
    a->setMenuRole(QAction::QuitRole);
    if (desktop::supported()) {
        auto* d = new QAction(QStringLiteral("Put an icon on the desktop"), m);
        m->insertAction(a, d);
        d->setToolTip(QStringLiteral("A LibreRamen launcher in the desktop folder"));
        connect(d, &QAction::triggered, this, [this] {
            const QString err = desktop::createLauncher();
            if (err.isEmpty())
                addLog(QStringLiteral("Desktop icon: %1").arg(desktop::launcherPath()));
            else if (dialogsEnabled())
                QMessageBox::warning(this, QStringLiteral("Desktop icon"), QStringLiteral("No desktop icon: %1").arg(err));
            else
                addLog(QStringLiteral("No desktop icon: %1").arg(err));
        });
        m->insertSeparator(a);
    }

    m = menuBar()->addMenu(QStringLiteral("&Instrument"));
    m->addAction(QStringLiteral("Use simulator"), this, [this] { connectInstrument(true); });
    m->addAction(QStringLiteral("Reconnect"), this, [this] { connectInstrument(false); });
    QAction* fac = m->addAction(QStringLiteral("Load factory calibration…"), this, &MainWindow::loadFactoryCalibration);
    fac->setToolTip(QStringLiteral("The instrument's BS_Device_Settings.ini from the vendor software (BS_System folder): "
                                   "its wavelength calibration, stored per serial number"));
    m_shutDownAction = m->addAction(QStringLiteral("Shut down instrument…"), this, &MainWindow::shutDownInstrument);
    m_shutDownAction->setToolTip(QStringLiteral("Laser off, then switch the instrument off (vendor shutdown)"));
    m_shutDownAction->setEnabled(false);
    m->addSeparator();
    QMenu* onClose = m->addMenu(QStringLiteral("When closing LibreRamen"));
    auto* grp = new QActionGroup(onClose);
    const std::pair<const char*, OnClose> choices[] = {{"Ask", OnClose::Ask},
                                                       {"Shut the instrument down", OnClose::ShutDown},
                                                       {"Leave the instrument on", OnClose::LeaveOn}};
    for (const auto& [label, policy] : choices) {
        QAction* c = onClose->addAction(QString::fromLatin1(label));
        c->setCheckable(true);
        grp->addAction(c);
        const OnClose p = policy;
        connect(c, &QAction::triggered, this, [p] { setOnClosePolicy(p); });
    }
    connect(onClose, &QMenu::aboutToShow, this, [grp] {      // reflect "Remember my choice"
        grp->actions().at(int(onClosePolicy()))->setChecked(true);
    });
    m->addSeparator();
    m_apiAction = m->addAction(QStringLiteral("Local API (command line, scripts, AI agents)"));
    m_apiAction->setCheckable(true);
    m_apiAction->setChecked(m_api->isRunning());
    m_apiAction->setToolTip(QStringLiteral("HTTP/JSON API on 127.0.0.1 for LibreRamenCLI --remote, Python and LibreRamenMCP"));
    connect(m_apiAction, &QAction::triggered, this, [this](bool on) {
        QSettings().setValue(QLatin1String(kApiEnabledKey), on);
        if (on)
            startApi();
        else
            stopApi();
    });
    m_remoteAction = m->addAction(QStringLiteral("Allow remote measurements"));
    m_remoteAction->setCheckable(true);
    m_remoteAction->setChecked(m_api->remoteControl());
    m_remoteAction->setToolTip(QStringLiteral("Let API clients (command line, scripts, AI agents) switch the laser on. "
                                              "ARM on the instrument is still required. Stop is always allowed."));
    connect(m_remoteAction, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(QLatin1String(kRemoteControlKey), on);
        m_api->setRemoteControl(on);
    });
#if defined(Q_OS_WIN)
    m->addSeparator();
    m->addAction(QStringLiteral("Repair USB drivers…"), this, &MainWindow::repairDrivers);
#endif

    m = menuBar()->addMenu(QStringLiteral("&Tools"));
    QAction* carbonAct = m->addAction(QStringLiteral("Carbon D/G analysis…"), this, [this] {
        auto* host = qobject_cast<DataPageHost*>(page(QStringLiteral("Data")));
        if (!host) return;
        showArea(int(pageAreas().indexOf(QStringLiteral("Data"))));
        host->dataPage()->carbonDialog();
    });
    carbonAct->setObjectName(QStringLiteral("carbonAction"));
    carbonAct->setToolTip(QStringLiteral("D/G bands of carbons (current spectrum of the Data area): I_D/I_G, L_a "
                                         "(532 nm), D′, 2D band"));

    m = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction* qs = m->addAction(QStringLiteral("Quick start"), this, [this] { showHelp(HelpWindow::Doc::QuickStart); });
    qs->setShortcut(QKeySequence::HelpContents);
    m->addAction(QStringLiteral("Manual"), this, [this] { showHelp(HelpWindow::Doc::Manual); });
}

void MainWindow::showHelp(HelpWindow::Doc d) {
    if (!m_help)
        m_help = new HelpWindow(this);
    m_help->showDoc(d);
    m_help->show();
    m_help->raise();
    m_help->activateWindow();
}

void MainWindow::fitToScreen() {
    // at most the available screen area (macOS: without menu bar and Dock)
    QScreen* scr = QGuiApplication::primaryScreen();
    if (!scr) {
        resize(1440, 900);
        return;
    }
    const QRect a = scr->availableGeometry();
    // large enough for the Measure page without scrolling (1080p at 100 % and 125 %, 1920×1200, Mac laptops)
    const int w = std::min(1680, int(a.width() * 0.94)), h = std::min(1000, int(a.height() * 0.94));
    resize(w, h);
    move(a.x() + (a.width() - w) / 2, a.y() + (a.height() - h) / 2);
}

Page* MainWindow::page(const QString& area) const {
    const int i = int(pageAreas().indexOf(area));
    return i >= 0 && i < int(m_pages.size()) ? m_pages[std::size_t(i)] : nullptr;
}

int MainWindow::currentArea() const { return m_stack->currentIndex(); }

void MainWindow::showArea(int k) {
    m_stack->setCurrentIndex(k);
    for (std::size_t i = 0; i < m_sideButtons.size(); ++i)
        m_sideButtons[i]->setChecked(int(i) == k);
    if (k >= 0 && k < int(m_pages.size()))
        m_pages[std::size_t(k)]->activated();
}

void MainWindow::addLog(const QString& msg) {
    m_log->appendPlainText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss  ")) + msg);
    if (m_api)
        m_api->recordLog(msg);
}

bool MainWindow::startApi(int port, const QString& apiFile) {
    const int p = port >= 0 ? port : QSettings().value(QLatin1String(kApiPortKey), int(lr::api::kDefaultPort)).toInt();
    QString err;
    const bool ok = m_api->start(quint16(p), apiFile.isEmpty() ? lr::api::defaultApiFile() : apiFile, &err);
    m_apiError = ok ? QString() : err;
    if (!ok)
        addLog(QStringLiteral("Local API not started (port %1): %2").arg(p).arg(err));
    updateRemoteTile();
    if (m_apiAction)
        m_apiAction->setChecked(ok);
    return ok;
}

void MainWindow::stopApi() {
    m_apiError.clear();
    if (m_api->isRunning()) {
        m_api->stop();
        addLog(QStringLiteral("Local API stopped"));
    }
    if (m_apiAction)
        m_apiAction->setChecked(false);
    updateRemoteTile();
}

void MainWindow::updateRemoteTile() {
    if (!m_api->isRunning() && !m_apiError.isEmpty()) {     // e.g. another LibreRamen owns the port
        m_strip->setRemote(StatusStrip::Remote::Off, QStringLiteral("not started"));
        m_strip->remoteTile()->setToolTip(QStringLiteral("Local API not started: ") + m_apiError);
    } else if (!m_api->isRunning()) {
        m_strip->setRemote(StatusStrip::Remote::Off, QStringLiteral("local API off"));
        m_strip->remoteTile()->setToolTip({});
    }
    if (m_api->isRunning()) {
        m_strip->remoteTile()->setToolTip({});
        m_strip->setRemote(m_api->remoteControl() ? StatusStrip::Remote::Control : StatusStrip::Remote::ReadOnly,
                           QStringLiteral("127.0.0.1:%1").arg(m_api->port()));
    }
    if (m_remoteAction && m_remoteAction->isChecked() != m_api->remoteControl())
        m_remoteAction->setChecked(m_api->remoteControl());
}

void MainWindow::onError(const QString& msg) {
    addLog(QStringLiteral("ERROR: ") + msg.section(QLatin1Char('\n'), 0, 0));
    if (dialogsEnabled())
        QMessageBox::warning(this, QStringLiteral("LibreRamen"), msg);
}

void MainWindow::updateTitle(bool laserOn) {
    if (laserOn == m_laserTitle)
        return;
    m_laserTitle = laserOn;
    setWindowTitle((laserOn ? QStringLiteral("● LASER ON – ") : QString()) + kTitle);
}

void MainWindow::onStatus(const StatusView& st) {
    m_strip->update(st);
    if (st.connection != QLatin1String("connected"))
        m_measure->setReady(false);
    updateTitle(st.laserOn);
    m_simBar->setVisible(st.simulated);
    if (m_shutDownAction)
        m_shutDownAction->setEnabled(st.connection == QLatin1String("connected") && m_ctl->instrument());
}

void MainWindow::onConnected(const InstrumentInfo& info) {
    m_info = info;
    m_measure->setReady(true);
    if (info.simulated && m_simSample)
        m_ctl->simSetSample(m_simSample->currentText());
    addLog(QStringLiteral("Instrument ready: %1 S/N %2, calibration %3").arg(info.model, info.serial, info.calibration));
}

void MainWindow::onConnectFailed(const QString& msg) {
    addLog(QStringLiteral("No instrument: ") + msg.section(QLatin1Char('\n'), 0, 0));
    if (!dialogsEnabled())
        return;
    // after the other receivers (main(): show this window and close the splash screen)
    QTimer::singleShot(0, this, [this, msg] {
        QMessageBox r(this);
        r.setWindowTitle(QStringLiteral("No Raman spectrometer found"));
        r.setIcon(QMessageBox::Warning);
        r.setText(QStringLiteral("<b>No Raman spectrometer found.</b>"));
        r.setInformativeText(QStringLiteral("Check: USB cable connected · instrument switched on and <i>On</i> "
                                            "pressed (wait until its screen is ready) · vendor software closed."
                                            "<br><br>") +
                             msg.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
        QPushButton* retry = r.addButton(QStringLiteral("Try again"), QMessageBox::AcceptRole);
        QPushButton* sim = r.addButton(QStringLiteral("Use simulator"), QMessageBox::ActionRole);
        r.addButton(QStringLiteral("Offline (analysis only)"), QMessageBox::RejectRole);
        r.setDefaultButton(retry);
        r.exec();
        if (r.clickedButton() == retry)
            connectInstrument(false);
        else if (r.clickedButton() == sim)
            connectInstrument(true);
    });
}

void MainWindow::onSpectrum(const lr::Spectrum& sp) {
    for (Page* p : m_pages)                           // Measure page: plot; Data page: add_to_data
        p->addSpectrum(sp);
}

void MainWindow::connectInstrument(bool simulate) {
    m_measure->setReady(false);
    m_info = {};
    m_ctl->connectInstrument(simulate);
}

void MainWindow::loadFactoryCalibration() {
    if (m_ctl->isBusy()) {
        if (dialogsEnabled())
            QMessageBox::information(this, QStringLiteral("Factory calibration"),
                                     QStringLiteral("Stop the measurement first."));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Factory calibration of the instrument (BS_Device_Settings.ini)"), QString(),
        QStringLiteral("Vendor settings (BS_Device_Settings.ini *.ini);;All files (*)"));
    if (path.isEmpty())
        return;
    try {
        const Calibration cal = importFactoryIni(path);
        addLog(QStringLiteral("Factory calibration of S/N %1 stored: %2").arg(cal.serial, cal.source));
        const bool connected = !m_info.serial.isEmpty() && !m_ctl->simulated();
        if (connected && m_info.serial != cal.serial) {
            if (dialogsEnabled())
                QMessageBox::warning(this, QStringLiteral("Factory calibration"),
                                     QStringLiteral("The file is for S/N %1, but the connected instrument is S/N %2. "
                                                    "It is stored and will be used when S/N %1 is connected.")
                                         .arg(cal.serial, m_info.serial));
            return;
        }
        if (connected)
            connectInstrument(false);             // reconnect: the stored factory calibration is selected on connect
        else if (dialogsEnabled())
            QMessageBox::information(this, QStringLiteral("Factory calibration"),
                                     QStringLiteral("Stored for S/N %1; it is used when that instrument is connected.")
                                         .arg(cal.serial));
    } catch (const std::exception& e) {
        if (dialogsEnabled())
            QMessageBox::warning(this, QStringLiteral("Factory calibration"), QString::fromUtf8(e.what()));
        addLog(QStringLiteral("Factory calibration: ") + QString::fromUtf8(e.what()));
    }
}

void MainWindow::repairDrivers() {
    if (m_ctl->isBusy()) {
        if (dialogsEnabled())
            QMessageBox::information(this, QStringLiteral("LibreRamen"), QStringLiteral("Stop the running measurement first."));
        return;
    }
    if (m_ctl->instrument() && !m_ctl->simulated()) {
        m_measure->setReady(false);
        m_ctl->disconnectInstrument();                // release the USB devices while their driver changes
    }
    std::shared_ptr<lr::windrivers::ElevatedProcess> proc;
    try {
        proc = lr::windrivers::repair();
    } catch (const lr::Error& e) {
        addLog(QStringLiteral("Driver repair: ") + QString::fromUtf8(e.what()));
        if (dialogsEnabled())
            QMessageBox::warning(this, QStringLiteral("LibreRamen"), QString::fromUtf8(e.what()));
        return;
    }
    addLog(QStringLiteral("Installing USB drivers…"));
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this, timer, proc] {
        const std::optional<int> code = proc->poll();
        if (!code)
            return;
        timer->stop();
        timer->deleteLater();
        addLog(QStringLiteral("USB driver set-up finished (code %1); reconnecting").arg(*code));
        QTimer::singleShot(3000, this, [this] { connectInstrument(false); });   // let Windows re-enumerate
    });
    timer->start(500);
}

MainWindow::OnClose MainWindow::onClosePolicy() {
    const QString v = QSettings().value(QLatin1String(kOnCloseKey), QStringLiteral("ask")).toString();
    if (v == QLatin1String("shutdown"))
        return OnClose::ShutDown;
    if (v == QLatin1String("leave"))
        return OnClose::LeaveOn;
    return OnClose::Ask;
}

void MainWindow::setOnClosePolicy(OnClose p) {
    QSettings().setValue(QLatin1String(kOnCloseKey), p == OnClose::ShutDown  ? QStringLiteral("shutdown")
                                                     : p == OnClose::LeaveOn ? QStringLiteral("leave")
                                                                             : QStringLiteral("ask"));
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (m_shutDown) {
        e->accept();
        return;
    }
    if (m_ctl->isBusy() && dialogsEnabled() &&
        QMessageBox::question(this, QStringLiteral("Quit"), QStringLiteral("A measurement is running. Stop it and quit?")) !=
            QMessageBox::Yes) {
        e->ignore();
        return;
    }
    bool powerDown = false;
    const bool real = m_ctl->instrument() && !m_ctl->simulated() &&
                      m_ctl->connection() == Controller::Connection::Connected;
    if (real) {
        const OnClose policy = onClosePolicy();
        if (policy == OnClose::ShutDown) {
            powerDown = true;
        } else if (policy == OnClose::Ask && dialogsEnabled()) {
            QMessageBox q(this);
            q.setWindowTitle(QStringLiteral("Quit"));
            q.setIcon(QMessageBox::Question);
            q.setText(QStringLiteral("<b>Switch the Raman spectrometer off?</b>"));
            q.setInformativeText(QStringLiteral(
                "<p><b>Shut down</b>: the instrument switches off (as its Off button). Next time it must be switched "
                "on and initialised again (about a minute).</p><p><b>Leave on</b>: it stays ready (spectrometers "
                "cooled) for the next start. The laser is off either way.</p>"));
            QPushButton* down = q.addButton(QStringLiteral("Shut down"), QMessageBox::AcceptRole);
            QPushButton* leave = q.addButton(QStringLiteral("Leave on"), QMessageBox::RejectRole);
            q.addButton(QMessageBox::Cancel);
            q.setDefaultButton(leave);
            auto* remember = new QCheckBox(QStringLiteral("Remember my choice (Instrument > When closing LibreRamen)"));
            q.setCheckBox(remember);
            q.exec();
            if (q.clickedButton() != down && q.clickedButton() != leave) {
                e->ignore();
                return;
            }
            powerDown = q.clickedButton() == down;
            if (remember->isChecked())
                setOnClosePolicy(powerDown ? OnClose::ShutDown : OnClose::LeaveOn);
        }
    }
    if (m_ctl->isBusy())
        addLog(QStringLiteral("Stopping the measurement…"));
    m_api->stop();
    m_ctl->shutdown(powerDown);
    m_shutDown = true;
    e->accept();
}

}  // namespace lr::gui

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "application.h"

#include "platform/platform_specific.h"
#include "mainwidget.h"
#include "mainwindow.h"
#include "storage/localstorage.h"
#include "window/notifications_manager.h"
#include "core/crash_reports.h"
#include "messenger.h"
#include "base/timer.h"
#include "base/concurrent_timer.h"
#include "base/qthelp_url.h"
#include "base/qthelp_regex.h"
#include "core/update_checker.h"
#include "core/crash_report_window.h"

namespace {

constexpr auto kEmptyPidForCommandResponse = 0ULL;

QChar _toHex(ushort v) {
	v = v & 0x000F;
	return QChar::fromLatin1((v >= 10) ? ('a' + (v - 10)) : ('0' + v));
}
ushort _fromHex(QChar c) {
	return ((c.unicode() >= uchar('a')) ? (c.unicode() - uchar('a') + 10) : (c.unicode() - uchar('0'))) & 0x000F;
}

QString _escapeTo7bit(const QString &str) {
	QString result;
	result.reserve(str.size() * 2);
	for (int i = 0, l = str.size(); i != l; ++i) {
		QChar ch(str.at(i));
		ushort uch(ch.unicode());
		if (uch < 32 || uch > 127 || uch == ushort(uchar('%'))) {
			result.append('%').append(_toHex(uch >> 12)).append(_toHex(uch >> 8)).append(_toHex(uch >> 4)).append(_toHex(uch));
		} else {
			result.append(ch);
		}
	}
	return result;
}

QString _escapeFrom7bit(const QString &str) {
	QString result;
	result.reserve(str.size());
	for (int i = 0, l = str.size(); i != l; ++i) {
		QChar ch(str.at(i));
		if (ch == QChar::fromLatin1('%') && i + 4 < l) {
			result.append(QChar(ushort((_fromHex(str.at(i + 1)) << 12) | (_fromHex(str.at(i + 2)) << 8) | (_fromHex(str.at(i + 3)) << 4) | _fromHex(str.at(i + 4)))));
			i += 4;
		} else {
			result.append(ch);
		}
	}
	return result;
}

} // namespace

bool InternalPassportLink(const QString &url) {
	const auto urlTrimmed = url.trimmed();
	if (!urlTrimmed.startsWith(qstr("tg://"), Qt::CaseInsensitive)) {
		return false;
	}
	const auto command = urlTrimmed.midRef(qstr("tg://").size());

	using namespace qthelp;
	const auto matchOptions = RegExOption::CaseInsensitive;
	const auto authMatch = regex_match(
		qsl("^passport/?\\?(.+)(#|$)"),
		command,
		matchOptions);
	const auto usernameMatch = regex_match(
		qsl("^resolve/?\\?(.+)(#|$)"),
		command,
		matchOptions);
	const auto usernameValue = usernameMatch->hasMatch()
		? url_parse_params(
			usernameMatch->captured(1),
			UrlParamNameTransform::ToLower).value(qsl("domain"))
		: QString();
	const auto authLegacy = (usernameValue == qstr("telegrampassport"));
	return authMatch->hasMatch() || authLegacy;
}

bool StartUrlRequiresActivate(const QString &url) {
	return Messenger::Instance().locked()
		? true
		: !InternalPassportLink(url);
}

Application::Application(
		not_null<Core::Launcher*> launcher,
		int &argc,
		char **argv)
: QApplication(argc, argv)
, _mainThreadId(QThread::currentThreadId())
, _launcher(launcher) {
}

int Application::execute() {
	if (!Core::UpdaterDisabled()) {
		_updateChecker = std::make_unique<Core::UpdateChecker>();
	}
	const auto d = QFile::encodeName(QDir(cWorkingDir()).absolutePath());
	char h[33] = { 0 };
	hashMd5Hex(d.constData(), d.size(), h);
#ifndef OS_MAC_STORE
	_localServerName = psServerPrefix() + h + '-' + cGUIDStr();
#else // OS_MAC_STORE
	h[4] = 0; // use only first 4 chars
	_localServerName = psServerPrefix() + h;
#endif // OS_MAC_STORE

	connect(&_localSocket, SIGNAL(connected()), this, SLOT(socketConnected()));
	connect(&_localSocket, SIGNAL(disconnected()), this, SLOT(socketDisconnected()));
	connect(&_localSocket, SIGNAL(error(QLocalSocket::LocalSocketError)), this, SLOT(socketError(QLocalSocket::LocalSocketError)));
	connect(&_localSocket, SIGNAL(bytesWritten(qint64)), this, SLOT(socketWritten(qint64)));
	connect(&_localSocket, SIGNAL(readyRead()), this, SLOT(socketReading()));
	connect(&_localServer, SIGNAL(newConnection()), this, SLOT(newInstanceConnected()));

	QTimer::singleShot(0, this, SLOT(startApplication()));
	connect(this, SIGNAL(aboutToQuit()), this, SLOT(closeApplication()));

	if (cManyInstance()) {
		LOG(("Many instance allowed, starting..."));
		singleInstanceChecked();
	} else {
        LOG(("Connecting local socket to %1...").arg(_localServerName));
		_localSocket.connectToServer(_localServerName);
	}

	return exec();
}

Application::~Application() = default;

bool Application::event(QEvent *e) {
	if (e->type() == QEvent::Close) {
		App::quit();
	}
	return QApplication::event(e);
}

void Application::socketConnected() {
	LOG(("Socket connected, this is not the first application instance, sending show command..."));
	_secondInstance = true;

	QString commands;
	const QStringList &lst(cSendPaths());
	for (QStringList::const_iterator i = lst.cbegin(), e = lst.cend(); i != e; ++i) {
		commands += qsl("SEND:") + _escapeTo7bit(*i) + ';';
	}
	if (!cStartUrl().isEmpty()) {
		commands += qsl("OPEN:") + _escapeTo7bit(cStartUrl()) + ';';
	} else {
		commands += qsl("CMD:show;");
	}

	DEBUG_LOG(("Application Info: writing commands %1").arg(commands));
	_localSocket.write(commands.toLatin1());
}

void Application::socketWritten(qint64/* bytes*/) {
	if (_localSocket.state() != QLocalSocket::ConnectedState) {
		LOG(("Socket is not connected %1").arg(_localSocket.state()));
		return;
	}
	if (_localSocket.bytesToWrite()) {
		return;
	}
	LOG(("Show command written, waiting response..."));
}

void Application::socketReading() {
	if (_localSocket.state() != QLocalSocket::ConnectedState) {
		LOG(("Socket is not connected %1").arg(_localSocket.state()));
		return;
	}
	_localSocketReadData.append(_localSocket.readAll());
	if (QRegularExpression("RES:(\\d+);").match(_localSocketReadData).hasMatch()) {
		uint64 pid = _localSocketReadData.mid(4, _localSocketReadData.length() - 5).toULongLong();
		if (pid != kEmptyPidForCommandResponse) {
			psActivateProcess(pid);
		}
		LOG(("Show command response received, pid = %1, activating and quitting...").arg(pid));
		return App::quit();
	}
}

void Application::socketError(QLocalSocket::LocalSocketError e) {
	if (App::quitting()) return;

	if (_secondInstance) {
		LOG(("Could not write show command, error %1, quitting...").arg(e));
		return App::quit();
	}

	if (e == QLocalSocket::ServerNotFoundError) {
		LOG(("This is the only instance of Telegram, starting server and app..."));
	} else {
		LOG(("Socket connect error %1, starting server and app...").arg(e));
	}
	_localSocket.close();

// Local server does not work in WinRT build.
#ifndef Q_OS_WINRT
	psCheckLocalSocket(_localServerName);

	if (!_localServer.listen(_localServerName)) {
		LOG(("Failed to start listening to %1 server, error %2").arg(_localServerName).arg(int(_localServer.serverError())));
		return App::quit();
	}
#endif // !Q_OS_WINRT

	if (!Core::UpdaterDisabled()
		&& !cNoStartUpdate()
		&& Core::checkReadyUpdate()) {
		cSetRestartingUpdate(true);
		DEBUG_LOG(("Application Info: installing update instead of starting app..."));
		return App::quit();
	}

	singleInstanceChecked();
}

void Application::singleInstanceChecked() {
	if (cManyInstance()) {
		Logs::multipleInstances();
	}

	Sandbox::start();
	refreshGlobalProxy();

	if (!Logs::started() || (!cManyInstance() && !Logs::instanceChecked())) {
		new NotStartedWindow();
	} else {
		const auto status = CrashReports::Start();
		if (status == CrashReports::CantOpen) {
			new NotStartedWindow();
		} else if (status == CrashReports::LastCrashed) {
			if (Sandbox::LastCrashDump().isEmpty()) { // don't handle bad closing for now
				if (CrashReports::Restart() == CrashReports::CantOpen) {
					new NotStartedWindow();
				} else {
					Sandbox::launch();
				}
			} else {
				new LastCrashedWindow();
			}
		} else {
			Sandbox::launch();
		}
	}
}

void Application::socketDisconnected() {
	if (_secondInstance) {
		DEBUG_LOG(("Application Error: socket disconnected before command response received, quitting..."));
		return App::quit();
	}
}

void Application::newInstanceConnected() {
	DEBUG_LOG(("Application Info: new local socket connected"));
	for (QLocalSocket *client = _localServer.nextPendingConnection(); client; client = _localServer.nextPendingConnection()) {
		_localClients.push_back(LocalClient(client, QByteArray()));
		connect(client, SIGNAL(readyRead()), this, SLOT(readClients()));
		connect(client, SIGNAL(disconnected()), this, SLOT(removeClients()));
	}
}

void Application::readClients() {
	// This method can be called before Messenger is constructed.
	QString startUrl;
	QStringList toSend;
	for (LocalClients::iterator i = _localClients.begin(), e = _localClients.end(); i != e; ++i) {
		i->second.append(i->first->readAll());
		if (i->second.size()) {
			QString cmds(QString::fromLatin1(i->second));
			int32 from = 0, l = cmds.length();
			for (int32 to = cmds.indexOf(QChar(';'), from); to >= from; to = (from < l) ? cmds.indexOf(QChar(';'), from) : -1) {
				QStringRef cmd(&cmds, from, to - from);
				if (cmd.startsWith(qsl("CMD:"))) {
					Sandbox::execExternal(cmds.mid(from + 4, to - from - 4));
					const auto response = qsl("RES:%1;").arg(QCoreApplication::applicationPid()).toLatin1();
					i->first->write(response.data(), response.size());
				} else if (cmd.startsWith(qsl("SEND:"))) {
					if (cSendPaths().isEmpty()) {
						toSend.append(_escapeFrom7bit(cmds.mid(from + 5, to - from - 5)));
					}
				} else if (cmd.startsWith(qsl("OPEN:"))) {
					auto activateRequired = true;
					if (cStartUrl().isEmpty()) {
						startUrl = _escapeFrom7bit(cmds.mid(from + 5, to - from - 5)).mid(0, 8192);
						activateRequired = StartUrlRequiresActivate(startUrl);
					}
					if (activateRequired) {
						Sandbox::execExternal("show");
					}
					const auto responsePid = activateRequired
						? QCoreApplication::applicationPid()
						: kEmptyPidForCommandResponse;
					const auto response = qsl("RES:%1;").arg(responsePid).toLatin1();
					i->first->write(response.data(), response.size());
				} else {
					LOG(("Application Error: unknown command %1 passed in local socket").arg(QString(cmd.constData(), cmd.length())));
				}
				from = to + 1;
			}
			if (from > 0) {
				i->second = i->second.mid(from);
			}
		}
	}
	if (!toSend.isEmpty()) {
		QStringList paths(cSendPaths());
		paths.append(toSend);
		cSetSendPaths(paths);
	}
	if (!cSendPaths().isEmpty()) {
		if (App::wnd()) {
			App::wnd()->sendPaths();
		}
	}
	if (!startUrl.isEmpty()) {
		cSetStartUrl(startUrl);
	}
	if (auto messenger = Messenger::InstancePointer()) {
		messenger->checkStartUrl();
	}
}

void Application::removeClients() {
	DEBUG_LOG(("Application Info: remove clients slot called, clients %1").arg(_localClients.size()));
	for (LocalClients::iterator i = _localClients.begin(), e = _localClients.end(); i != e;) {
		if (i->first->state() != QLocalSocket::ConnectedState) {
			DEBUG_LOG(("Application Info: removing client"));
			i = _localClients.erase(i);
			e = _localClients.end();
		} else {
			++i;
		}
	}
}

void Application::startApplication() {
	if (App::quitting()) {
		quit();
	}
}

void Application::createMessenger() {
	Expects(!App::quitting());

	_messengerInstance = std::make_unique<Messenger>(_launcher);

	// Ideally this should go to constructor.
	// But we want to catch all native events and Messenger installs
	// its own filter that can filter out some of them. So we install
	// our filter after the Messenger constructor installs his.
	installNativeEventFilter(this);
}

void Application::refreshGlobalProxy() {
#ifndef TDESKTOP_DISABLE_NETWORK_PROXY
	const auto proxy = [&] {
		if (Global::started()) {
			return (Global::ProxySettings() == ProxyData::Settings::Enabled)
				? Global::SelectedProxy()
				: ProxyData();
		}
		return Sandbox::PreLaunchProxy();
	}();
	if (proxy.type == ProxyData::Type::Socks5
		|| proxy.type == ProxyData::Type::Http) {
		QNetworkProxy::setApplicationProxy(
			ToNetworkProxy(ToDirectIpProxy(proxy)));
	} else if (!Global::started()
		|| Global::ProxySettings() == ProxyData::Settings::System) {
		QNetworkProxyFactory::setUseSystemConfiguration(true);
	} else {
		QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
	}
#endif // TDESKTOP_DISABLE_NETWORK_PROXY
}

void Application::postponeCall(FnMut<void()> &&callable) {
	Expects(callable != nullptr);
	Expects(_eventNestingLevel >= _loopNestingLevel);

	// _loopNestingLevel == _eventNestingLevel means that we had a
	// native event in a nesting loop that didn't get a notify() call
	// after. That means we already have exited the nesting loop and
	// there must not be any postponed calls with that nesting level.
	if (_loopNestingLevel == _eventNestingLevel) {
		Assert(_postponedCalls.empty()
			|| _postponedCalls.back().loopNestingLevel < _loopNestingLevel);
		Assert(!_previousLoopNestingLevels.empty());

		_loopNestingLevel = _previousLoopNestingLevels.back();
		_previousLoopNestingLevels.pop_back();
	}

	_postponedCalls.push_back({
		_loopNestingLevel,
		std::move(callable)
	});
}

void Application::incrementEventNestingLevel() {
	++_eventNestingLevel;
}

void Application::decrementEventNestingLevel() {
	if (_eventNestingLevel == _loopNestingLevel) {
		_loopNestingLevel = _previousLoopNestingLevels.back();
		_previousLoopNestingLevels.pop_back();
	}
	const auto processTillLevel = _eventNestingLevel - 1;
	processPostponedCalls(processTillLevel);
	_eventNestingLevel = processTillLevel;
}

void Application::registerEnterFromEventLoop() {
	if (_eventNestingLevel > _loopNestingLevel) {
		_previousLoopNestingLevels.push_back(_loopNestingLevel);
		_loopNestingLevel = _eventNestingLevel;
	}
}

bool Application::notify(QObject *receiver, QEvent *e) {
	if (QThread::currentThreadId() != _mainThreadId) {
		return QApplication::notify(receiver, e);
	}

	const auto wrap = createEventNestingLevel();
	return QApplication::notify(receiver, e);
}

void Application::processPostponedCalls(int level) {
	while (!_postponedCalls.empty()) {
		auto &last = _postponedCalls.back();
		if (last.loopNestingLevel != level) {
			break;
		}
		auto taken = std::move(last);
		_postponedCalls.pop_back();
		taken.callable();
	}
}

bool Application::nativeEventFilter(
		const QByteArray &eventType,
		void *message,
		long *result) {
	registerEnterFromEventLoop();
	return false;
}

void Application::activateWindowDelayed(not_null<QWidget*> widget) {
	if (_delayedActivationsPaused) {
		return;
	} else if (std::exchange(_windowForDelayedActivation, widget.get())) {
		return;
	}
	crl::on_main(this, [=] {
		if (const auto widget = base::take(_windowForDelayedActivation)) {
			if (!widget->isHidden()) {
				widget->activateWindow();
			}
		}
	});
}

void Application::pauseDelayedWindowActivations() {
	_windowForDelayedActivation = nullptr;
	_delayedActivationsPaused = true;
}

void Application::resumeDelayedWindowActivations() {
	_delayedActivationsPaused = false;
}

void Application::closeApplication() {
	if (App::launchState() == App::QuitProcessed) return;
	App::setLaunchState(App::QuitProcessed);

	_messengerInstance.reset();

	Sandbox::finish();

	_localServer.close();
	for (LocalClients::iterator i = _localClients.begin(), e = _localClients.end(); i != e; ++i) {
		disconnect(i->first, SIGNAL(disconnected()), this, SLOT(removeClients()));
		i->first->close();
	}
	_localClients.clear();

	_localSocket.close();

	_updateChecker = nullptr;
}

namespace Sandbox {

void execExternal(const QString &cmd) {
	DEBUG_LOG(("Application Info: executing external command '%1'").arg(cmd));
	if (cmd == "show") {
		if (App::wnd()) {
			App::wnd()->activate();
		} else if (PreLaunchWindow::instance()) {
			PreLaunchWindow::instance()->activate();
		}
	}
}

void launch() {
	const auto dpi = Application::primaryScreen()->logicalDotsPerInch();
	LOG(("Primary screen DPI: %1").arg(dpi));
	if (dpi <= 108) {
		cSetScreenScale(100); // 100%:  96 DPI (0-108)
	} else if (dpi <= 132) {
		cSetScreenScale(125); // 125%: 120 DPI (108-132)
	} else if (dpi <= 168) {
		cSetScreenScale(150); // 150%: 144 DPI (132-168)
	} else if (dpi <= 216) {
		cSetScreenScale(200); // 200%: 192 DPI (168-216)
	} else if (dpi <= 264) {
		cSetScreenScale(250); // 250%: 240 DPI (216-264)
	} else {
		cSetScreenScale(300); // 300%: 288 DPI (264-inf)
	}

	auto devicePixelRatio = Core::App().devicePixelRatio();
	if (devicePixelRatio > 1.) {
		if ((cPlatform() != dbipMac && cPlatform() != dbipMacOld) || (devicePixelRatio != 2.)) {
			LOG(("Found non-trivial Device Pixel Ratio: %1").arg(devicePixelRatio));
			LOG(("Environmental variables: QT_DEVICE_PIXEL_RATIO='%1'").arg(QString::fromLatin1(qgetenv("QT_DEVICE_PIXEL_RATIO"))));
			LOG(("Environmental variables: QT_SCALE_FACTOR='%1'").arg(QString::fromLatin1(qgetenv("QT_SCALE_FACTOR"))));
			LOG(("Environmental variables: QT_AUTO_SCREEN_SCALE_FACTOR='%1'").arg(QString::fromLatin1(qgetenv("QT_AUTO_SCREEN_SCALE_FACTOR"))));
			LOG(("Environmental variables: QT_SCREEN_SCALE_FACTORS='%1'").arg(QString::fromLatin1(qgetenv("QT_SCREEN_SCALE_FACTORS"))));
		}
		cSetRetinaFactor(devicePixelRatio);
		cSetIntRetinaFactor(int32(cRetinaFactor()));
		cSetScreenScale(kInterfaceScaleDefault);
	}

	Core::App().createMessenger();
}

} // namespace Sandbox

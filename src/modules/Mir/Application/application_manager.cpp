/*
 * Copyright (C) 2013 Canonical, Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 3, as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranties of MERCHANTABILITY,
 * SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// local
#include "application_manager.h"
#include "application.h"
#include "desktopfilereader.h"
#include "dbuswindowstack.h"

// unity-mir
#include "mirserverconfiguration.h"
#include "nativeinterface.h"
#include "sessionlistener.h"
#include "sessionauthorizer.h"
#include "taskcontroller.h"
#include "logging.h"

// mir
#include <mir/scene/surface.h>
#include <mir/scene/session.h>
#include <mir/shell/focus_controller.h>

// Qt
#include <QGuiApplication>
#include <QDebug>

namespace ms = mir::scene;

using namespace unity::shell::application;

ApplicationManager *ApplicationManager::the_application_manager = nullptr;

ApplicationManager* ApplicationManager::singleton()
{
    if (!the_application_manager) {
        the_application_manager = new ApplicationManager();
    }
    return the_application_manager;
}

ApplicationManager::ApplicationManager(QObject *parent)
:   ApplicationManagerInterface(parent)
,   m_focusedApplication(nullptr)
,   m_applicationToBeFocused(nullptr)
,   m_lifecycleExceptions(QStringList() << "com.ubuntu.music")
,   m_taskController(TaskController::singleton())
,   m_fenceNext(false)
{
    DLOG("ApplicationManager::ApplicationManager (this=%p)", this);

    m_roleNames.insert(RoleSurface, "surface");
    m_roleNames.insert(RoleFullscreen, "fullscreen");

    NativeInterface *nativeInterface = dynamic_cast<NativeInterface*>(QGuiApplication::platformNativeInterface());

    m_mirConfig = nativeInterface->m_mirConfig;

    if (!nativeInterface) {
        LOG("ERROR: Unity.Application QML plugin requires use of the 'mirserver' QPA plugin");
        QGuiApplication::quit();
        return;
    }

    SessionListener *sessionListener = static_cast<SessionListener*>(nativeInterface->nativeResourceForIntegration("SessionListener"));
    SessionAuthorizer *sessionAuthorizer = static_cast<SessionAuthorizer*>(nativeInterface->nativeResourceForIntegration("SessionAuthorizer"));
    qDebug() << sessionListener << sessionAuthorizer;
    QObject::connect(sessionListener, &SessionListener::sessionStarting,
                     this, &ApplicationManager::onSessionStarting);
    QObject::connect(sessionListener, &SessionListener::sessionStopping,
                     this, &ApplicationManager::onSessionStopping);
    QObject::connect(sessionListener, &SessionListener::sessionFocused,
                     this, &ApplicationManager::onSessionFocused);
    QObject::connect(sessionListener, &SessionListener::sessionUnfocused,
                     this, &ApplicationManager::onSessionUnfocused);
    QObject::connect(sessionListener, &SessionListener::sessionCreatedSurface,
                     this, &ApplicationManager::onSessionCreatedSurface);
    QObject::connect(sessionAuthorizer, &SessionAuthorizer::requestAuthorizationForSession,
                     this, &ApplicationManager::authorizeSession, Qt::BlockingQueuedConnection);

    QObject::connect(m_taskController.data(), &TaskController::processStartReport,
                     this, &ApplicationManager::onProcessStartReportReceived);
    QObject::connect(m_taskController.data(), &TaskController::processStopped,
                     this, &ApplicationManager::onProcessStopped);
    QObject::connect(m_taskController.data(), &TaskController::requestFocus,
                     this, &ApplicationManager::onFocusRequested);
    QObject::connect(m_taskController.data(), &TaskController::requestResume,
                     this, &ApplicationManager::onResumeRequested);

    m_dbusWindowStack = new DBusWindowStack(this);
}

ApplicationManager::~ApplicationManager()
{
    DLOG("ApplicationManager::~ApplicationManager");
}

int ApplicationManager::rowCount(const QModelIndex &parent) const
{
    return !parent.isValid() ? m_applications.size() : 0;
}

QVariant ApplicationManager::data(const QModelIndex &index, int role) const
{
    if (index.row() >= 0 && index.row() < m_applications.size()) {
        Application *application = m_applications.at(index.row());
        switch (role) {
            case RoleAppId:
                return QVariant::fromValue(application->appId());
            case RoleName:
                return QVariant::fromValue(application->name());
            case RoleComment:
                return QVariant::fromValue(application->comment());
            case RoleIcon:
                return QVariant::fromValue(application->icon());
            case RoleStage:
                return QVariant::fromValue((int)application->stage());
            case RoleState:
                return QVariant::fromValue((int)application->state());
            case RoleFocused:
                return QVariant::fromValue(application->focused());
            case RoleScreenshot:
                return QVariant::fromValue(application->screenshot());
            case RoleSurface:
                return QVariant::fromValue(application->surface());
            case RoleFullscreen:
                return QVariant::fromValue(application->fullscreen());
            default:
                return QVariant();
        }
    } else {
        return QVariant();
    }
}

Application* ApplicationManager::get(int index) const
{
    DLOG("ApplicationManager::get (this=%p, index=%i, count=%i)", this, index, m_applications.count());
    if (index < 0 || index >= m_applications.count()) {
        return nullptr;
    }
    return m_applications.at(index);
}

Application* ApplicationManager::findApplication(const QString &appId) const
{
    for (Application *app : m_applications) {
        if (app->appId() == appId) {
            return app;
        }
    }
    return nullptr;
}

bool ApplicationManager::requestFocusApplication(const QString &appId)
{
    DLOG("ApplicationManager::requestFocusApplication (this=%p, appId=%s)", this, qPrintable(appId));
    Application *application = findApplication(appId);

    if (!application) {
        DLOG("No such running application '%s'", qPrintable(appId));
        return false;
    }

    if (application == m_focusedApplication) {
        DLOG("Application %s is already focused", qPrintable(appId));
        return true;
    }

    // Update the screenshot for the currently focused app
    Application *currentlyFocusedApplication = findApplication(focusedApplicationId());
    if (currentlyFocusedApplication) {
        m_nextFocusedAppId = appId;
        currentlyFocusedApplication->updateScreenshot();
    } else {
        Q_EMIT focusRequested(appId);
    }
    return true;
}

QString ApplicationManager::focusedApplicationId() const
{
    if (m_focusedApplication) {
        return m_focusedApplication->appId();
    } else {
        return QString();
    }
}

bool ApplicationManager::suspended() const
{
    return m_suspended;
}

void ApplicationManager::setSuspended(bool suspended)
{
    if (suspended == m_suspended) {
        return;
    }
    m_suspended = suspended;
    Q_EMIT suspendedChanged();

    if (m_suspended) {
        suspendApplication(m_focusedApplication);
    } else {
        resumeApplication(m_focusedApplication);
    }
}

void ApplicationManager::suspendApplication(Application *application)
{
    if (application == nullptr)
        return;
    DLOG("ApplicationManager::suspendApplication (appId=%s)", qPrintable(application->appId()));

    updateScreenshot(application->appId());

    // Present in exceptions list, return.
    if (!m_lifecycleExceptions.filter(application->appId().section('_',0,0)).empty())
        return;

    if (application->state() == Application::Running)
        application->setState(Application::Suspended);
}

void ApplicationManager::resumeApplication(Application *application)
{
    if (application == nullptr)
        return;

    if (application->state() != Application::Running)
        application->setState(Application::Running);
}

bool ApplicationManager::focusApplication(const QString &appId)
{
    DLOG("ApplicationManager::focusApplication (this=%p, appId=%s)", this, qPrintable(appId));
    Application *application = findApplication(appId);

    if (!application) {
        DLOG("No such running application '%s'", qPrintable(appId));
        return false;
    }

    m_applicationToBeFocused = application;

    if (application->state() == Application::Stopped) {
        // Respawning this app, move to end of application list so onSessionStarting works ok
        // FIXME: this happens pretty late, shell could request respawn earlier
        application->setState(Application::Running);
        int from = m_applications.indexOf(application);
        move(from, m_applications.length()-1);
    } else {
        if (application->session()) {
            m_mirConfig->the_focus_controller()->set_focus_to(application->session());
            int from = m_applications.indexOf(application);
            move(from, 0);
        }
    }

    setFocused(application);
    QModelIndex appIndex = findIndex(application);
    Q_EMIT dataChanged(appIndex, appIndex, QVector<int>() << RoleFocused);

    // FIXME(dandrader): lying here. The operation is async. So we will only know whether
    // the focusing was successful once the server replies. Maybe the API in unity-api should
    // reflect that?
    return true;
}

void ApplicationManager::unfocusCurrentApplication()
{
    DLOG("ApplicationManager::unfocusCurrentApplication (this=%p)", this);

    m_applicationToBeFocused = nullptr;

    m_mirConfig->the_focus_controller()->set_focus_to(NULL); //FIXME(greyback)
}

Application* ApplicationManager::startApplication(const QString &appId,
                                                  const QStringList &arguments)
{
    return startApplication(appId, NoFlag, arguments);
}

Application *ApplicationManager::startApplication(const QString &appId, ExecFlags flags,
                                                  const QStringList &arguments)
{
    DLOG("ApplicationManager::startApplication (this=%p, appId=%s)", this, qPrintable(appId));

    if (!m_taskController->start(appId, arguments)) {
        LOG("Asking Upstart to start application '%s' failed", qPrintable(appId));
        return nullptr;
    }

    Application* application = new Application(appId, Application::Starting, arguments, this);
    if (!application->isValid()) {
        DLOG("Unable to instantiate application with appId '%s'", qPrintable(appId));
        return nullptr;
    }

    // override stage if necessary
    if (application->stage() == Application::SideStage && flags.testFlag(ApplicationManager::ForceMainStage)) {
        application->setStage(Application::MainStage);
    }

    add(application);
    return application;
}

void ApplicationManager::onProcessStartReportReceived(const QString &appId, const bool failure)
{
    DLOG("ApplicationManager::onProcessStartReportReceived (this=%p, appId=%s, failure=%c)",
         this, qPrintable(appId), (failure) ? 'Y' : 'N');

    if (failure) {
        onProcessStopped(appId, true);
    }

    Application *application = findApplication(appId);

    if (!application) { // if shell did not start this application, but upstart did
        application = new Application(appId, Application::Starting, QStringList(), this);
        if (!application->isValid()) {
            DLOG("Unable to instantiate application with appId '%s'", qPrintable(appId));
            return;
        }
        add(application);
        Q_EMIT focusRequested(appId);
    }
}

bool ApplicationManager::stopApplication(const QString &appId)
{
    DLOG("ApplicationManager::stopApplication (this=%p, appId=%s)", this, qPrintable(appId));

    Application *application = findApplication(appId);

    if (!application) {
        DLOG("No such running application '%s'", qPrintable(appId));
        return false;
    }

    if (application == m_focusedApplication) {
        // TODO(greyback) What to do?? Focus next app, or unfocus everything??
        m_focusedApplication = NULL;
        Q_EMIT focusedApplicationIdChanged();
    }

    remove(application);
    m_dbusWindowStack->WindowDestroyed(0, application->appId());

    bool result = m_taskController->stop(application->appId());

    LOG_IF(result == false, "FAILED to ask Upstart to stop application '%s'", qPrintable(application->appId()));
    delete application;

    // FIXME(dandrader): lying here. The operation is async. So we will only know whether
    // the focusing was successful once the server replies. Maybe the API in unity-api should
    // reflect that?
    return result;
}

bool ApplicationManager::updateScreenshot(const QString &appId)
{
    Application *application = findApplication(appId);
    if (!application) {
        DLOG("No such running application '%s'", qPrintable(appId));
        return false;
    }

    application->updateScreenshot();
    QModelIndex appIndex = findIndex(application);
    Q_EMIT dataChanged(appIndex, appIndex, QVector<int>() << RoleScreenshot);
    return true;
}

void ApplicationManager::onProcessStopped(const QString &appId, const bool unexpected)
{
    Application *application = findApplication(appId);

    // if shell did not stop the application, but upstart says it died, we assume the process has been
    // killed, so it can be respawned later. Only exception is if that application is focused or running
    // as then it most likely crashed. Update this logic when upstart gives some failure info.
    if (application) {
        bool removeApplication = false;

        if (application == m_focusedApplication) {
            // Very bad case where focused application dies. Remove from list. Should give error message
            m_focusedApplication = nullptr;
            Q_EMIT focusedApplicationIdChanged();
            removeApplication = true;
        }

        if (application->state() == Application::Running || application->state() == Application::Starting) {
            // Application probably crashed, else OOM killer struck. Either way state wasn't saved
            // so just remove application
            removeApplication = true;
        } else if (application->state() == Application::Suspended) {
            application->setState(Application::Stopped);
            application->setSession(nullptr);
        }

        if (removeApplication) {
            remove(application);
            m_dbusWindowStack->WindowDestroyed(0, application->appId());
            delete application;
        }
    }

    if (unexpected) {
        // TODO: pop up a message box/notification?
        LOG("ApplicationManager: application '%s' died unexpectedly!", qPrintable(appId));
    }
}

void ApplicationManager::onFocusRequested(const QString& appId)
{
    DLOG("ApplicationManager::onFocusRequested (this=%p, appId=%s)", this, qPrintable(appId));

    Q_EMIT focusRequested(appId);
}

void ApplicationManager::onResumeRequested(const QString& appId)
{
    DLOG("ApplicationManager::onResumeRequested (this=%p, appId=%s)", this, qPrintable(appId));

    Application *application = findApplication(appId);

    if (!application) {
        DLOG("ApplicationManager::onResumeRequested: No such running application '%s'", qPrintable(appId));
        return;
    }

    // If app Stopped, trust that upstart-app-launch respawns it itself, and AppManager will
    // be notified of that through the onProcessStartReportReceived slot. Else resume.
    if (application->state() == Application::Suspended) {
        application->setState(Application::Running);
    }
}

void ApplicationManager::screenshotUpdated()
{
    Application *application = static_cast<Application*>(sender());
    QModelIndex appIndex = findIndex(application);
    Q_EMIT dataChanged(appIndex, appIndex, QVector<int>() << RoleScreenshot);

    DLOG("updated screenshot for '%s'", qPrintable(application->appId()));

    if (!m_nextFocusedAppId.isEmpty()) {
        Q_EMIT focusRequested(m_nextFocusedAppId);
        m_nextFocusedAppId.clear();
    }
}

/************************************* Mir-side methods *************************************/

void ApplicationManager::authorizeSession(const quint64 pid, bool &authorized)
{
    authorized = false; //to be proven wrong

    DLOG("ApplicationManager::authorizeSession (this=%p, pid=%lld)", this, pid);

    for (Application *app : m_applications) {
        if (app->state() == Application::Starting
                && m_taskController->appIdHasProcessId(app->appId(), pid)) {
            app->setPid(pid);
            authorized = true;
            return;
        }
    }

    /*
     * Hack: Allow applications to be launched externally, but must be executed with the
     * "desktop_file_hint" parameter attached. This exists until upstart-app-launch can
     * notify shell it is starting an application and so shell should allow it. Also reads
     * the --stage parameter to determine the desired stage
     */
    QFile cmdline(QString("/proc/%1/cmdline").arg(pid));
    if (!cmdline.open(QIODevice::ReadOnly | QIODevice::Text)) {
        DLOG("ApplicationManager REJECTED connection from app with pid %lld as unable to read process command", pid);
        return;
    }

    QByteArray command = cmdline.readLine().replace('\0', ' ');

    // FIXME: special exception for the OSK - maliit-server - not very secure
    if (command.startsWith("maliit-server") || command.startsWith("/usr/lib/arm-linux-gnueabihf/qt5/libexec/QtWebProcess")
        || command.startsWith("/usr/bin/signon-ui")) {
        authorized = true;
        m_fenceNext = true;
        return;
    }

    QString pattern = QRegularExpression::escape("--desktop_file_hint=") + "(\\S+)";
    QRegularExpression regExp(pattern);
    QRegularExpressionMatch regExpMatch = regExp.match(command);

    if (!regExpMatch.hasMatch()) {
        LOG("ApplicationManager REJECTED connection from app with pid %lld as no desktop_file_hint specified", pid);
        return;
    }

    QString desktopFileName = regExpMatch.captured(1);
    DLOG("Process supplied desktop_file_hint, loading '%s'", desktopFileName.toLatin1().data());

    // FIXME: right now we support --desktop_file_hint=appId for historical reasons. So let's try that in
    // case we didn't get an existing .desktop file path
    DesktopFileReader* desktopData;
    if (QFileInfo(desktopFileName).exists()) {
        desktopData = new DesktopFileReader(QFileInfo(desktopFileName));
    } else {
        desktopData = new DesktopFileReader(desktopFileName);
    }

    if (!desktopData->loaded()) {
        delete desktopData;
        LOG("ApplicationManager REJECTED connection from app with pid %lld as desktop_file_hint file not found", pid);
        return;
    }

    // some naughty applications use a script to launch the actual application. Check for the
    // case where shell actually launched the script.
    Application *application = findApplication(desktopData->appId());
    if (application && application->state() == Application::Starting) {
        DLOG("Process with pid %lld appeared, attached to existing entry '%s' in application lists",
             pid, application->appId().toLatin1().data());
        delete desktopData;
        application->setSessionName(application->appId());
        application->setPid(pid);
        authorized = true;
        return;
    }

    // if stage supplied in CLI, fetch that
    Application::Stage stage = Application::MainStage;
    pattern = QRegularExpression::escape("--stage=") + "(\\S+)";
    regExp.setPattern(pattern);
    regExpMatch = regExp.match(command);

    if (regExpMatch.hasMatch() && regExpMatch.captured(1) == "side_stage") {
        stage = Application::SideStage;
    }

    DLOG("Existing process with pid %lld appeared, adding '%s' to application lists", pid, desktopData->name().toLatin1().data());

    QString argStr(command.data());
    QStringList arguments(argStr.split(' '));
    application = new Application(desktopData, Application::Starting, arguments, this);
    application->setPid(pid);
    application->setStage(stage);
    add(application);
    authorized = true;
}

void ApplicationManager::onSessionStarting(std::shared_ptr<ms::Session> const& session)
{
    DLOG("ApplicationManager::onSessionStarting (this=%p, application=%s)", this, session->name().c_str());

    if (m_fenceNext) {
        m_fenceNext = false;
        return;
    }

    Application* application = findApplicationWithPid(session->process_id());
    if (application && application->state() != Application::Running) {
        application->setSession(session);
        m_applicationToBeFocused = application;
    } else {
        DLOG("ApplicationManager::onSessionStarting - unauthorized application!!");
    }
}

void ApplicationManager::onSessionStopping(std::shared_ptr<ms::Session> const& session)
{
    DLOG("ApplicationManager::onSessionStopping (this=%p, application=%s)", this, session->name().c_str());

    // in case application closed not by hand of shell, check again here:
    Application* application = findApplicationWithSession(session);
    if (application) {
        bool removeApplication = true;

        if (application->state() != Application::Starting) {
            application->setState(Application::Stopped);
            application->setSession(nullptr);
            m_dbusWindowStack->WindowDestroyed(0, application->appId());
            if (application != m_focusedApplication) {
                   removeApplication = false;
            }
        }

        if (removeApplication) {
            // TODO(greyback) What to do?? Focus next app, or unfocus everything??
            m_focusedApplication = NULL;
            remove(application);
            delete application;
            Q_EMIT focusedApplicationIdChanged();
        }
    }
}

void ApplicationManager::onSessionFocused(std::shared_ptr<ms::Session> const& session)
{
    DLOG("ApplicationManager::onSessionFocused (this=%p, application=%s)", this, session->name().c_str());
    Application* application = findApplicationWithSession(session);

    // Don't give application focus until it has created it's surface, when it is set as state "Running"
    // and only notify shell of focus changes that it actually expects
    if (application && application->state() != Application::Starting && application == m_applicationToBeFocused
            && application != m_focusedApplication) {
        setFocused(application);
        QModelIndex appIndex = findIndex(application);
        Q_EMIT dataChanged(appIndex, appIndex, QVector<int>() << RoleFocused);
    } else {
        if (application == nullptr) {
            DLOG("Invalid application focused, discarding the event");
            if (NULL != m_focusedApplication)
                focusApplication(m_focusedApplication->appId());
        }
    }
}

void ApplicationManager::onSessionUnfocused()
{
    DLOG("ApplicationManager::onSessionUnfocused (this=%p)", this);
    if (NULL != m_focusedApplication) {
        Q_ASSERT(m_focusedApplication->focused());
        m_focusedApplication->setFocused(false);

        //suspendApplication(m_focusedApplication);

        m_focusedApplication = NULL;
        Q_EMIT focusedApplicationIdChanged();
        m_dbusWindowStack->FocusedWindowChanged(0, QString(), 0);

        QModelIndex appIndex = findIndex(m_focusedApplication);
        Q_EMIT dataChanged(appIndex, appIndex, QVector<int>() << RoleFocused << RoleState);
    }
}

void ApplicationManager::onSessionCreatedSurface(ms::Session const* session,
                                               std::shared_ptr<ms::Surface> const& surface)
{
    DLOG("ApplicationManager::onSessionCreatedSurface (this=%p)", this);
    Q_UNUSED(surface);

    Application* application = findApplicationWithSession(session);
    if (application && application->state() == Application::Starting) {
        m_dbusWindowStack->WindowCreated(0, application->appId());
        m_focusedApplication->setState(Application::Running);
    }
}

void ApplicationManager::setFocused(Application *application)
{
    DLOG("ApplicationManager::setFocused (appId=%s)", qPrintable(application->appId()));

    if (application == m_focusedApplication)
        return;

    // set state of previously focused app to suspended
    suspendApplication(m_focusedApplication);


    m_focusedApplication = application;
    m_focusedApplication->setFocused(true);
    move(m_applications.indexOf(application), 0);
    Q_EMIT focusedApplicationIdChanged();
    m_dbusWindowStack->FocusedWindowChanged(0, application->appId(), application->stage());
}

Application* ApplicationManager::findApplicationWithSession(const std::shared_ptr<ms::Session> &session)
{
    return findApplicationWithSession(session.get());
}

Application* ApplicationManager::findApplicationWithSession(const ms::Session *session)
{
    for (Application *app : m_applications) {
        if (app->session().get() == session) {
            return app;
        }
    }
    return nullptr;
}

Application* ApplicationManager::findApplicationWithPid(const qint64 pid)
{
    if (pid <= 0)
        return nullptr;

    for (Application *app : m_applications) {
        if (app->m_pid == pid) {
            return app;
        }
    }
    return nullptr;
}

void ApplicationManager::add(Application* application)
{
    DASSERT(application != NULL);
    DLOG("ApplicationManager::add (this=%p, application='%s')", this, qPrintable(application->name()));

    connect(application, &Application::screenshotChanged, this, &ApplicationManager::screenshotUpdated);

    beginInsertRows(QModelIndex(), 0, 0);
    m_applications.prepend(application);
    endInsertRows();
    emit countChanged();
    emit applicationAdded(application->appId());
    emit topmostApplicationChanged(application);
    if (m_applications.size() == 1) {
        emit emptyChanged();
    }
}

void ApplicationManager::remove(Application *application)
{
    DASSERT(application != NULL);
    DLOG("ApplicationManager::remove (this=%p, application='%s')", this, qPrintable(application->name()));

    int i = m_applications.indexOf(application);
    if (i != -1) {
        beginRemoveRows(QModelIndex(), i, i);
        m_applications.removeAt(i);
        endRemoveRows();
        emit applicationRemoved(application->appId());
        emit countChanged();
        if (i == 0) {
            emit topmostApplicationChanged(topmostApplication());
            emit emptyChanged();
        }
    }
}

void ApplicationManager::move(int from, int to) {
    DLOG("ApplicationManager::move (this=%p, from=%d, to=%d)", this, from, to);
    if (from == to) return;

    if (from >= 0 && from < m_applications.size() && to >= 0 && to < m_applications.size()) {
        QModelIndex parent;
        /* When moving an item down, the destination index needs to be incremented
           by one, as explained in the documentation:
           http://qt-project.org/doc/qt-5.0/qtcore/qabstractitemmodel.html#beginMoveRows */

        Application *oldTopmost = topmostApplication();
        beginMoveRows(parent, from, from, parent, to + (to > from ? 1 : 0));
        m_applications.move(from, to);
        endMoveRows();
        if (topmostApplication() != oldTopmost) {
            emit topmostApplicationChanged(topmostApplication());
        }
    }
    DLOG("ApplicationManager::move after (%s)", qPrintable(toString()));
}

QModelIndex ApplicationManager::findIndex(Application* application)
{
    for (int i = 0; i < m_applications.size(); ++i) {
        if (m_applications.at(i) == application) {
            return index(i);
        }
    }

    return QModelIndex();
}

QString ApplicationManager::toString() const
{
    QString result;
    for (int i = 0; i < m_applications.count(); ++i) {
        if (i > 0) {
            result.append(",");
        }
        result.append(m_applications.at(i)->appId());
    }
    return result;
}

Application* ApplicationManager::topmostApplication() const
{
    if (m_applications.isEmpty()) {
        return nullptr;
    } else {
        return m_applications[0];
    }
}

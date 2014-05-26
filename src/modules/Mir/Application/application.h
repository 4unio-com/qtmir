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

#ifndef APPLICATION_H
#define APPLICATION_H

// std
#include <memory>

//Qt
#include <QtCore/QtCore>
#include <QImage>

// Unity API
#include <unity/shell/application/ApplicationInfoInterface.h>

class QImage;
class DesktopFileReader;
class MirSurfaceItem;
class TaskController;
namespace mir { namespace scene { class Session; }}

class Application : public unity::shell::application::ApplicationInfoInterface {
    Q_FLAGS(Orientation SupportedOrientations)

    Q_OBJECT
    Q_PROPERTY(QString desktopFile READ desktopFile CONSTANT)
    Q_PROPERTY(QString exec READ exec CONSTANT)
    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY fullscreenChanged)
    Q_PROPERTY(Stage stage READ stage WRITE setStage NOTIFY stageChanged)
    Q_PROPERTY(SupportedOrientations supportedOrientations READ supportedOrientations CONSTANT)

public:
    // Matching Qt::ScreenOrientation values for convenience
    enum Orientation {
        PortraitOrientation = 0x1,
        LandscapeOrientation = 0x2,
        InvertedPortraitOrientation = 0x4,
        InvertedLandscapeOrientation = 0x8
    };
    Q_DECLARE_FLAGS(SupportedOrientations, Orientation)

    Application(const QString &appId, State state, const QStringList &arguments, QObject *parent = 0);
    Application(DesktopFileReader *desktopFileReader, State state, const QStringList &arguments, QObject *parent = 0);
    virtual ~Application();

    // ApplicationInfoInterface
    QString appId() const override;
    QString name() const override;
    QString comment() const override;
    QUrl icon() const override;
    Stage stage() const override;
    State state() const override;
    bool focused() const override;
    QUrl screenshot() const override;

    void setStage(Stage stage);

    QImage screenshotImage() const;
    void updateScreenshot();

    bool isValid() const;
    QString desktopFile() const;
    QString exec() const;
    bool fullscreen() const;
    std::shared_ptr<mir::scene::Session> session() const;

    SupportedOrientations supportedOrientations() const;

public Q_SLOTS:
    void suspend();
    void resume();
    void respawn();

Q_SIGNALS:
    void fullscreenChanged();
    void stageChanged(Stage stage);

    void surfaceCreated(MirSurfaceItem *surface);
    void surfaceAboutToBeDestroyed(MirSurfaceItem *surface);
    void surfaceDestroyed(MirSurfaceItem *surface);

private:
    pid_t pid() const;
    void setPid(pid_t pid);
    void setState(State state);
    void setFocused(bool focus);
    void setFullscreen(bool fullscreen);
    void setSession(const std::shared_ptr<mir::scene::Session>& session);
    void setSessionName(const QString& name);

    // FIXME: This is a hack. Remove once we have a real implementation for knowning
    // the supported orientations of an app
    void deduceSupportedOrientationsFromAppId();

    DesktopFileReader* m_desktopData;
    qint64 m_pid;
    Stage m_stage;
    State m_state;
    bool m_focused;
    QUrl m_screenshot;
    QImage m_screenshotImage;
    bool m_fullscreen;
    std::shared_ptr<mir::scene::Session> m_session;
    QString m_sessionName;
    QStringList m_arguments;
    QTimer* m_suspendTimer;
    SupportedOrientations m_supportedOrientations;

    friend class ApplicationManager;
    friend class ApplicationListModel;
    friend class MirSurfaceManager;
};

Q_DECLARE_METATYPE(Application*)
Q_DECLARE_OPERATORS_FOR_FLAGS(Application::SupportedOrientations)

#endif  // APPLICATION_H

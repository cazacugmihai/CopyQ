/*
    Copyright (c) 2012, Lukas Holecek <hluk@email.cz>

    This file is part of CopyQ.

    CopyQ is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    CopyQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with CopyQ.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "clipboardmonitor.h"
#include "clipboardserver.h"
#include "clipboarditem.h"
#include "client_server.h"

#include <QMimeData>
#include <QTimer>

#ifdef Q_WS_X11
#include <X11/Xlib.h>
#endif

class PrivateX11 {
#ifdef Q_WS_X11
public:
    PrivateX11()
        : m_dsp(NULL)
        , m_timer()
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(100);
    }

    ~PrivateX11()
    {
        if (m_dsp)
            XCloseDisplay(m_dsp);
    }

    bool waitForKeyRelease()
    {
        if (m_timer.isActive())
            return true;

        if (m_dsp == NULL) {
            m_dsp = XOpenDisplay(NULL);
            if (m_dsp == NULL)
                return false;
        }

        XEvent event;
        Window root = DefaultRootWindow(m_dsp);
        XQueryPointer(m_dsp, root,
                      &event.xbutton.root, &event.xbutton.window,
                      &event.xbutton.x_root, &event.xbutton.y_root,
                      &event.xbutton.x, &event.xbutton.y,
                      &event.xbutton.state);

        if( event.xbutton.state & (Button1Mask | ShiftMask) ) {
            m_timer.start();
            return true;
        }

        return false;
    }

    const QTimer *timer() const
    {
        return &m_timer;
    }

private:
    Display *m_dsp;
    QTimer m_timer;
#endif
};

ClipboardMonitor::ClipboardMonitor(int &argc, char **argv)
    : App(argc, argv)
    , m_formats()
    , m_newdata(NULL)
    , m_checkclip(false)
    , m_copyclip(false)
    , m_checksel(false)
    , m_copysel(false)
    , m_lastHash(0)
    , m_socket( new QLocalSocket(this) )
    , m_updateTimer( new QTimer(this) )
#ifdef Q_WS_X11
    , m_x11(new PrivateX11)
#else
    , m_x11(NULL)
#endif
{
    connect( m_socket, SIGNAL(readyRead()),
             this, SLOT(readyRead()), Qt::DirectConnection );
    connect( m_socket, SIGNAL(disconnected()),
             this, SLOT(quit()) );
    m_socket->connectToServer( ClipboardServer::monitorServerName() );
    if ( !m_socket->waitForConnected(2000) ) {
        log( tr("Cannot connect to server!"), LogError );
        exit(1);
    }

    m_updateTimer->setSingleShot(true);
    m_updateTimer->setInterval(500);
    connect( m_updateTimer, SIGNAL(timeout()),
             this, SLOT(updateTimeout()), Qt::DirectConnection);

    connect( QApplication::clipboard(), SIGNAL(changed(QClipboard::Mode)),
             this, SLOT(checkClipboard(QClipboard::Mode)) );

#ifdef Q_WS_X11
    connect( m_x11->timer(), SIGNAL(timeout()),
             this, SLOT(updateSelection()) );
#endif
}

ClipboardMonitor::~ClipboardMonitor()
{
    delete m_x11;
}

void ClipboardMonitor::setFormats(const QString &list)
{
    m_formats = list.split( QRegExp("[;,\\s]+") );
}

#ifdef Q_WS_X11
bool ClipboardMonitor::updateSelection(bool check)
{
    // wait while selection is incomplete, i.e. mouse button or
    // shift key is pressed
    if ( m_x11->waitForKeyRelease() )
        return false;

    if (check)
        checkClipboard(QClipboard::Selection);

    return true;
}

void ClipboardMonitor::checkClipboard(QClipboard::Mode mode)
{
    const QMimeData *data;
    uint newHash;

    // check if clipboard data are needed
    if (mode == QClipboard::Clipboard) {
        if ( (!m_checkclip && !m_copyclip) ||
             QApplication::clipboard()->ownsClipboard() )
        {
            return;
        }
    } else if (mode == QClipboard::Selection) {
        if ( (!m_checksel && !m_copysel) ||
             QApplication::clipboard()->ownsSelection() ||
             !updateSelection(false) )
        {
            return;
        }
        // clipboard has priority
        QApplication::processEvents();
    } else {
        return;
    }

    // get clipboard data
    data = clipboardData(mode);

    // data retrieved?
    if (!data) {
        log( tr("Cannot access clipboard data!"), LogError );
        return;
    }

    // same data as last time?
    newHash = hash(*data, m_formats);
    if (m_lastHash == newHash)
        return;

    // clone only mime types defined by user
    data = cloneData(*data, &m_formats);
    // any data found?
    if ( data->formats().isEmpty() ) {
        delete data;
        return;
    }

    // send data to serve and synchronize if needed
    m_lastHash = newHash;
    if (mode == QClipboard::Clipboard) {
        if (m_checkclip)
            clipboardChanged(mode, cloneData(*data));
        if (m_copyclip)
            setClipboardData( cloneData(*data), QClipboard::Selection );
    } else {
        if (m_checksel)
            clipboardChanged(mode, cloneData(*data));
        if (m_copysel)
            setClipboardData( cloneData(*data), QClipboard::Clipboard );
    }

    delete data;
}
#else /* !Q_WS_X11 */
void ClipboardMonitor::checkClipboard(QClipboard::Mode mode)
{
    const QMimeData *data;
    QMimeData *data2;
    uint newHash;

    // check if clipboard data are needed
    if (mode != QClipboard::Clipboard || !m_checkclip ||
            QApplication::clipboard()->ownsClipboard())
    {
        return;
    }

    // get clipboard data
    data = clipboardData(mode);

    // data retrieved?
    if (!data) {
        log( tr("Cannot access clipboard data!"), LogError );
        return;
    }

    // same data as last time?
    newHash = hash(*data, m_formats);
    if (m_lastHash == newHash)
        return;

    // clone only mime types defined by user
    data2 = cloneData(*data, &m_formats);
    // any data found?
    if ( data2->formats().isEmpty() ) {
        delete data2;
        return;
    }

    // send data to serve and synchronize if needed
    m_lastHash = newHash;

    clipboardChanged(mode, data2);
}

bool ClipboardMonitor::updateSelection(bool)
{
    return true;
}
#endif

void ClipboardMonitor::clipboardChanged(QClipboard::Mode, QMimeData *data)
{
    ClipboardItem item;

    item.setData(data);

    // send clipboard item
    QByteArray msg;
    QDataStream out(&msg, QIODevice::WriteOnly);
    out << item;
    writeMessage(m_socket, msg);
}

void ClipboardMonitor::updateTimeout()
{
    if (m_newdata)
        updateClipboard(m_newdata, true);
}

void ClipboardMonitor::readyRead()
{
    m_socket->blockSignals(true);
    while ( m_socket->bytesAvailable() ) {
        QByteArray msg;
        if( !readMessage(m_socket, &msg) ) {
            log( tr("Cannot read message from server!"), LogError );
            return;
        }

        ClipboardItem item;
        QDataStream in(&msg, QIODevice::ReadOnly);
        in >> item;

        /* Does server send settings for monitor? */
        QByteArray settings_data = item.data()->data("application/x-copyq-settings");
        if ( !settings_data.isEmpty() ) {
            QDataStream settings_in(settings_data);
            QVariantMap settings;
            settings_in >> settings;

            if ( m_lastHash == 0 && settings.contains("_last_hash") )
                m_lastHash = settings["_last_hash"].toUInt();
            if ( settings.contains("formats") )
                setFormats( settings["formats"].toString() );
            if ( settings.contains("check_clipboard") )
                setCheckClipboard( settings["check_clipboard"].toBool() );
#ifdef Q_WS_X11
            if ( settings.contains("copy_clipboard") )
                setCopyClipboard( settings["copy_clipboard"].toBool() );
            if ( settings.contains("copy_selection") )
                setCopySelection( settings["copy_selection"].toBool() );
            if ( settings.contains("check_selection") )
                setCheckSelection( settings["check_selection"].toBool() );

            checkClipboard(QClipboard::Selection);
#endif
            checkClipboard(QClipboard::Clipboard);
        } else {
            updateClipboard( cloneData(*item.data()) );
        }
    }
    m_socket->blockSignals(false);
}

void ClipboardMonitor::updateClipboard(QMimeData *data, bool force)
{
    if (m_newdata && m_newdata != data)
        delete m_newdata;

    m_newdata = data;
    if ( !force && m_updateTimer->isActive() )
        return;

    m_lastHash = hash(*data, data->formats());
    setClipboardData(data, QClipboard::Clipboard);
#ifdef Q_WS_X11
    setClipboardData(cloneData(*data), QClipboard::Selection);
#endif

    m_newdata = NULL;

    m_updateTimer->start();
}


/*
   Copyright (C) 1999-2006 Id Software, Inc. and contributors.
   For a list of contributors, see the accompanying CONTRIBUTORS file.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "console.h"

#include <ctime>
#include <array>
#include <deque>
#include <string>
#include <algorithm>
#include <vector>

#include "gtkutil/accelerator.h"
#include "gtkutil/messagebox.h"
#include "stream/stringstream.h"
#include "signal/signal.h"

#include "aboutmsg.h"
#include "mainframe.h"
#include "version.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

#ifndef WIN32
#include <unistd.h> // write()
#endif

// handle to the console log file
namespace {
FILE *g_hLogFile;
}

// called whenever we need to open/close/check the console log file
void Sys_LogFile(bool enable) {
  if (enable && !g_hLogFile) {
    // we should be logging and we don't have a log file .. so create it
    if (string_empty(SettingsPath_get())) {
      return; // cannot open a log file yet
    }
    // open a file to log the console
    // the file handle is g_hLogFile
    // the log file is erased
    const auto name = StringStream(SettingsPath_get(), "viberadiant.log");
    g_hLogFile = fopen(name, "w");
    if (g_hLogFile != 0) {
      globalOutputStream() << "Started logging to " << name << '\n';
      time_t localtime;
      time(&localtime);
      globalOutputStream() << "Today is: " << ctime(&localtime)
                           << "This is VibeRadiant '" RADIANT_VERSION
                              "' compiled " __DATE__ "\n" RADIANT_ABOUTMSG "\n";
    } else {
      qt_MessageBox(0,
                    "Failed to create log file, check write permissions in "
                    "VibeRadiant directory.\n",
                    "Console logging", EMessageBoxType::Error);
    }
  } else if (!enable && g_hLogFile != 0) {
    // we should not be logging but still we have an active logfile .. close it
    time_t localtime;
    time(&localtime);
    globalOutputStream() << "Closing log file at " << ctime(&localtime) << '\n';
    fclose(g_hLogFile);
    g_hLogFile = 0;
  }
}

namespace
{
QPlainTextEdit* g_console = nullptr;
Signal0 g_consoleSummaryChanged;

enum class ConsoleSummaryBucket : std::size_t
{
	Notifications = 0,
	Warnings,
	Errors,
	Count
};

struct ConsoleSummaryState
{
	int unreadCount = 0;
	std::deque<std::string> recentLines;
	std::string pendingLine;
	std::vector<std::string> allLines;
};

std::array<ConsoleSummaryState, static_cast<std::size_t>( ConsoleSummaryBucket::Count )> g_summaryState;
constexpr std::size_t c_summaryRecentLinesLimit = 8;

class QPlainTextEdit_console : public QPlainTextEdit
{
protected:
	void contextMenuEvent( QContextMenuEvent *event ) override {
		QMenu *menu = createStandardContextMenu();
		connect( menu->addAction( "Copy All" ), &QAction::triggered,
		         [this](){ QApplication::clipboard()->setText( toPlainText() ); } );
		connect( menu->addAction( "Clear" ), &QAction::triggered, this,
		         &QPlainTextEdit::clear );
		menu->exec( event->globalPos() );
		delete menu;
	}
};

ConsoleSummaryState& console_bucket( ConsoleSummaryBucket bucket ){
	return g_summaryState[static_cast<std::size_t>( bucket )];
}

ConsoleSummaryBucket console_bucket_for_level( int level ){
	switch( level )
	{
	case SYS_ERR:
		return ConsoleSummaryBucket::Errors;
	case SYS_WRN:
		return ConsoleSummaryBucket::Warnings;
	case SYS_STD:
	default:
		return ConsoleSummaryBucket::Notifications;
	}
}

ConsoleSummaryBucket console_bucket_for_category( ConsoleSummaryCategory category ){
	switch( category )
	{
	case ConsoleSummaryCategory::Warnings:
		return ConsoleSummaryBucket::Warnings;
	case ConsoleSummaryCategory::Errors:
		return ConsoleSummaryBucket::Errors;
	case ConsoleSummaryCategory::Notifications:
	default:
		return ConsoleSummaryBucket::Notifications;
	}
}

void console_clear_summary_state(){
	for( auto& bucket : g_summaryState )
	{
		bucket.unreadCount = 0;
		bucket.recentLines.clear();
		bucket.pendingLine.clear();
		bucket.allLines.clear();
	}
}

void console_add_line( ConsoleSummaryState& state, const std::string& line ){
	if( line.empty() ){
		return;
	}
	++state.unreadCount;
	state.recentLines.push_back( line );
	while( state.recentLines.size() > c_summaryRecentLinesLimit )
		state.recentLines.pop_front();
	state.allLines.push_back( line );
}

bool console_track_summary( int level, const char* buffer, std::size_t length ){
	if( level == SYS_VRB || level == SYS_NOCON || buffer == nullptr || length == 0 ){
		return false;
	}

	ConsoleSummaryState& state = console_bucket( console_bucket_for_level( level ) );
	bool changed = false;

	for( std::size_t i = 0; i < length; ++i )
	{
		const char c = buffer[i];
		if( c == '\r' ){
			continue;
		}
		if( c == '\n' ){
			console_add_line( state, state.pendingLine );
			if( !state.pendingLine.empty() ){
				changed = true;
			}
			state.pendingLine.clear();
			continue;
		}
		state.pendingLine.push_back( c );
	}
	return changed;
}

std::string console_build_tooltip( const char* title, const ConsoleSummaryState& state ){
	StringOutputStream text( 2048 );
	text << title << ": " << state.unreadCount << '\n';
	if( state.unreadCount == 0 ){
		text << "No entries.";
		return text.c_str();
	}
	text << "Recent unread entries:";
	for( const auto& line : state.recentLines )
	{
		text << "\n - " << line.c_str();
	}
	return text.c_str();
}

class ConsolePane : public QWidget
{
	QToolButton* m_toggleButton = nullptr;
	QPlainTextEdit_console* m_text = nullptr;
	bool m_collapsed = false;
	QPointer<QWidget> m_overlayHost;
	int m_overlayExpandedHeight = 220;

	void updateOverlayGeometry(){
		if( m_overlayHost.isNull() ){
			return;
		}

		constexpr int margin = 8;
		const QSize buttonSize = m_toggleButton->sizeHint();
		const int hostWidth = std::max( 1, m_overlayHost->width() );
		const int hostHeight = std::max( 1, m_overlayHost->height() );

		if( m_collapsed ){
			const int x = margin;
			const int y = std::max( margin, hostHeight - buttonSize.height() - margin );
			setGeometry( x, y, buttonSize.width(), buttonSize.height() );
		}
		else{
			const int width = std::max( buttonSize.width(), hostWidth - margin * 2 );
			const int minHeight = buttonSize.height() + 80;
			const int maxHeight = std::max( minHeight, hostHeight - margin * 2 );
			const int height = std::clamp( m_overlayExpandedHeight, minHeight, maxHeight );
			const int x = margin;
			const int y = std::max( margin, hostHeight - height - margin );
			setGeometry( x, y, width, height );
		}

		raise();
	}

	void updateCollapsedUi(){
		m_toggleButton->setArrowType( m_collapsed ? Qt::UpArrow : Qt::DownArrow );
		m_toggleButton->setToolTip( m_collapsed ? "Expand console" : "Collapse console" );
		m_text->setVisible( !m_collapsed );

		if( m_overlayHost.isNull() ){
			const int collapsedHeight = m_toggleButton->sizeHint().height() + 8;
			setMinimumHeight( m_collapsed ? collapsedHeight : 10 );
			setMaximumHeight( m_collapsed ? collapsedHeight : QWIDGETSIZE_MAX );
		}
		else{
			setMinimumHeight( 0 );
			setMaximumHeight( QWIDGETSIZE_MAX );
			updateOverlayGeometry();
		}
	}

public:
	ConsolePane(){
		auto *vbox = new QVBoxLayout( this );
		vbox->setContentsMargins( 0, 0, 0, 0 );
		vbox->setSpacing( 4 );

		m_toggleButton = new QToolButton( this );
		m_toggleButton->setAutoRaise( true );
		m_toggleButton->setSizePolicy( QSizePolicy::Policy::Fixed, QSizePolicy::Policy::Fixed );
		vbox->addWidget( m_toggleButton, 0, Qt::AlignmentFlag::AlignLeft | Qt::AlignmentFlag::AlignTop );

		m_text = new QPlainTextEdit_console;
		m_text->setReadOnly( true );
		m_text->setUndoRedoEnabled( false );
		m_text->setMinimumHeight( 10 );
		m_text->setFocusPolicy( Qt::FocusPolicy::NoFocus );
		vbox->addWidget( m_text, 1 );

		QObject::connect( m_toggleButton, &QToolButton::clicked, [this](){
			setCollapsed( !m_collapsed );
		} );

		updateCollapsedUi();
	}

	bool eventFilter( QObject* watched, QEvent* event ) override {
		if( watched == m_overlayHost ){
			switch( event->type() )
			{
			case QEvent::Resize:
			case QEvent::Move:
			case QEvent::Show:
			case QEvent::LayoutRequest:
				updateOverlayGeometry();
				break;
			default:
				break;
			}
		}
		return QWidget::eventFilter( watched, event );
	}

	QPlainTextEdit_console* textEdit() const {
		return m_text;
	}

	bool isCollapsed() const {
		return m_collapsed;
	}

	void setCollapsed( bool collapsed ){
		if( m_collapsed == collapsed ){
			return;
		}

		m_collapsed = collapsed;
		updateCollapsedUi();
		g_consoleSummaryChanged();
	}

	void setOverlayHost( QWidget* host ){
		if( m_overlayHost == host ){
			updateCollapsedUi();
			return;
		}

		if( !m_overlayHost.isNull() ){
			m_overlayHost->removeEventFilter( this );
		}

		m_overlayHost = host;

		if( !m_overlayHost.isNull() ){
			setParent( m_overlayHost );
			m_overlayHost->installEventFilter( this );
			show();
			raise();
		}

		updateCollapsedUi();
	}
};

ConsolePane* g_consolePane = nullptr;
}

QWidget* Console_constructWindow(){
	auto *pane = new ConsolePane;
	g_consolePane = pane;
	g_console = pane->textEdit();

	// globalExtendedASCIICharacterSet().print();

	QObject::connect( pane, &QObject::destroyed, [](){
		g_consolePane = nullptr;
		g_console = nullptr;
		console_clear_summary_state();
		g_consoleSummaryChanged();
	} );

	return pane;
}

// #pragma GCC push_options
// #pragma GCC optimize ("O0")

class GtkTextBufferOutputStream : public TextOutputStream {
  QPlainTextEdit *textBuffer;

public:
  GtkTextBufferOutputStream(QPlainTextEdit *textBuffer)
      : textBuffer(textBuffer) {}
  std::size_t
#ifdef __GNUC__
//__attribute__((optimize("O0")))
#endif
  write(const char *buffer, std::size_t length) override {
    textBuffer->insertPlainText(QString::fromLatin1(buffer, length));
    return length;
  }
};

// #pragma GCC pop_options

std::size_t Sys_Print(int level, const char *buf, std::size_t length) {
  const bool contains_newline =
      std::find(buf, buf + length, '\n') != buf + length;

  if (level == SYS_ERR) {
    Sys_LogFile(true);
  }

  if (g_hLogFile != 0) {
    fwrite(buf, 1, length, g_hLogFile);
    if (contains_newline) {
      fflush(g_hLogFile);
    }
  }

  if (level != SYS_NOCON) {
    const bool summaryChanged = console_track_summary(level, buf, length);
#ifndef WIN32
    { // on linux/macos log also to terminal
      switch (level) {
      case SYS_WRN:
      case SYS_ERR:
        write(2, buf, length);
        break;
      case SYS_STD:
      case SYS_VRB:
      default:
        write(1, buf, length);
        break;
      }
    }
#endif

    if (g_console != 0) {
      g_console->moveCursor(
          QTextCursor::End); // must go before setCurrentCharFormat() &
                             // insertPlainText()

      {
        QTextCharFormat format = g_console->currentCharFormat();
        switch (level) {
        case SYS_WRN:
          format.setForeground(QColor(255, 127, 0));
          break;
        case SYS_ERR:
          format.setForeground(QColor(255, 0, 0));
          break;
        case SYS_STD:
        case SYS_VRB:
        default:
          format.clearForeground();
          break;
        }
        g_console->setCurrentCharFormat(format);
      }

      {
        GtkTextBufferOutputStream textBuffer(g_console);
        textBuffer << StringRange(buf, length);
      }

      if (contains_newline) {
        g_console->ensureCursorVisible();

        // update console widget immediately if we're doing something
        // time-consuming
        if (!ScreenUpdates_Enabled() && g_console->isVisible()) {
          ScreenUpdates_process();
        }
      }
    }
    if (summaryChanged) {
      g_consoleSummaryChanged();
    }
  }
  return length;
}

bool Console_isCollapsed(){
	return g_consolePane != nullptr && g_consolePane->isCollapsed();
}

void Console_setCollapsed( bool collapsed ){
	if( g_consolePane != nullptr ){
		g_consolePane->setCollapsed( collapsed );
	}
}

void Console_setOverlayHost( QWidget* host ){
	if( g_consolePane != nullptr ){
		g_consolePane->setOverlayHost( host );
	}
}

ConsoleSummary Console_getSummary(){
	ConsoleSummary summary;
	const auto& notifications = console_bucket( ConsoleSummaryBucket::Notifications );
	const auto& warnings = console_bucket( ConsoleSummaryBucket::Warnings );
	const auto& errors = console_bucket( ConsoleSummaryBucket::Errors );

	summary.notifications = notifications.unreadCount;
	summary.warnings = warnings.unreadCount;
	summary.errors = errors.unreadCount;
	summary.notificationsTooltip = console_build_tooltip( "Notifications", notifications );
	summary.warningsTooltip = console_build_tooltip( "Warnings", warnings );
	summary.errorsTooltip = console_build_tooltip( "Errors", errors );
	return summary;
}

std::vector<std::string> Console_getCategoryMessages( ConsoleSummaryCategory category ){
	return console_bucket( console_bucket_for_category( category ) ).allLines;
}

void Console_clearCategory( ConsoleSummaryCategory category ){
	ConsoleSummaryState& state = console_bucket( console_bucket_for_category( category ) );
	state.unreadCount = 0;
	state.recentLines.clear();
	g_consoleSummaryChanged();
}

SignalHandlerId Console_summaryChanged_connect( const SignalHandler& handler ){
	return g_consoleSummaryChanged.connectLast( handler );
}

void Console_summaryChanged_disconnect( SignalHandlerId id ){
	if( !id.isNull() && g_consoleSummaryChanged.isConnected( id ) ){
		g_consoleSummaryChanged.disconnect( id );
	}
}

template <int level> class SysPrintStream : public TextOutputStream {
public:
  std::size_t write(const char *buffer, std::size_t length) override {
    return Sys_Print(level, buffer, length);
  }
};

TextOutputStream &getSysPrintOutputStream() {
  static SysPrintStream<SYS_STD> stream;
  return stream;
}

TextOutputStream &getSysPrintWarningStream() {
  static SysPrintStream<SYS_WRN> stream;
  return stream;
}

TextOutputStream &getSysPrintErrorStream() {
  static SysPrintStream<SYS_ERR> stream;
  return stream;
}

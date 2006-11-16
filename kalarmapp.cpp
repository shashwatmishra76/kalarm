/*
 *  kalarmapp.cpp  -  the KAlarm application object
 *  Program:  kalarm
 *  Copyright © 2001-2006 by David Jarvie <software@astrojar.org.uk>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "kalarm.h"

#include <stdlib.h>
#include <ctype.h>
#include <iostream>

#include <QObject>
#include <QTimer>
#include <QRegExp>
#include <QFile>
#include <QByteArray>
#include <QTextStream>

#include <kcmdlineargs.h>
#include <klocale.h>
#include <kstandarddirs.h>
#include <kconfig.h>
#include <kaboutdata.h>
#include <kprocess.h>
#include <ktemporaryfile.h>
#include <kfileitem.h>
#include <kstdguiitem.h>
#include <kservicetypetrader.h>
#include <kstaticdeleter.h>
#include <kdebug.h>

#include "alarmcalendar.h"
#include "alarmlistview.h"
#include "editdlg.h"
#include "daemon.h"
#include "dbushandler.h"
#include "functions.h"
#include "kamail.h"
#include "karecurrence.h"
#include "mainwindow.h"
#include "messagebox.h"
#include "messagewin.h"
#include "preferences.h"
#include "prefdlg.h"
#include "shellprocess.h"
#include "traywindow.h"
#include "kalarmapp.moc"

#include <netwm.h>
#include <kglobal.h>


static bool convWakeTime(const QByteArray& timeParam, KDateTime&, const KDateTime& defaultDt = KDateTime());
static bool convInterval(QByteArray timeParam, KARecurrence::Type&, int& timeInterval, bool allowMonthYear = true);

/******************************************************************************
 * Find the maximum number of seconds late which a late-cancel alarm is allowed
 * to be. This is calculated as the alarm daemon's check interval, plus a few
 * seconds leeway to cater for any timing irregularities.
 */
static inline int maxLateness(int lateCancel)
{
	static const int LATENESS_LEEWAY = 5;
	int lc = (lateCancel >= 1) ? (lateCancel - 1)*60 : 0;
	return Daemon::maxTimeSinceCheck() + LATENESS_LEEWAY + lc;
}


KAlarmApp*  KAlarmApp::theInstance  = 0;
int         KAlarmApp::mActiveCount = 0;
int         KAlarmApp::mFatalError  = 0;
QString     KAlarmApp::mFatalMessage;


/******************************************************************************
 * Construct the application.
 */
KAlarmApp::KAlarmApp()
	: KUniqueApplication(),
	  mInitialised(false),
	  mDBusHandler(new DBusHandler()),
	  mTrayWindow(0),
	  mPendingQuit(false),
	  mProcessingQueue(false),
	  mCheckingSystemTray(false),
	  mSessionClosingDown(false),
	  mRefreshArchivedAlarms(false),
	  mSpeechEnabled(false)
{
	Preferences::initialise();
	Preferences::connect(SIGNAL(preferencesChanged()), this, SLOT(slotPreferencesChanged()));
	KARecurrence::setDefaultFeb29Type(Preferences::defaultFeb29Type());

	// Check if the system tray is supported by this window manager
	mHaveSystemTray = true;   // assume yes in lieu of a test which works

	if (AlarmCalendar::initialiseCalendars())
	{
		connect(AlarmCalendar::resources(), SIGNAL(purged()), SLOT(slotArchivedPurged()));

		KConfig* config = KGlobal::config();
		config->setGroup(QLatin1String("General"));
		mNoSystemTray           = config->readEntry("NoSystemTray", false);
		mSavedNoSystemTray      = mNoSystemTray;
		mOldRunInSystemTray     = wantRunInSystemTray();
		mDisableAlarmsIfStopped = mOldRunInSystemTray && !mNoSystemTray && Preferences::disableAlarmsIfStopped();
		mStartOfDay             = Preferences::startOfDay();
		if (Preferences::hasStartOfDayChanged())
			mStartOfDay.setHMS(100,0,0);    // start of day time has changed: flag it as invalid
		mPrefsArchivedColour   = Preferences::archivedColour();
		mPrefsArchivedKeepDays = Preferences::archivedKeepDays();
		mPrefsShowTime         = Preferences::showAlarmTime();
		mPrefsShowTimeTo       = Preferences::showTimeToAlarm();
	}

	// Check if the speech synthesis daemon is installed
	mSpeechEnabled = (KServiceTypeTrader::self()->query("DBUS/Text-to-Speech", "Name == 'KTTSD'").count() > 0);
	if (!mSpeechEnabled)
		kDebug(5950) << "KAlarmApp::KAlarmApp(): speech synthesis disabled (KTTSD not found)" << endl;
	// Check if KOrganizer is installed
	QString korg = QLatin1String("korganizer");
	mKOrganizerEnabled = !KStandardDirs::locate("exe", korg).isNull()  ||  !KStandardDirs::findExe(korg).isNull();
	if (!mKOrganizerEnabled)
		kDebug(5950) << "KAlarmApp::KAlarmApp(): KOrganizer options disabled (KOrganizer not found)" << endl;
}

/******************************************************************************
*/
KAlarmApp::~KAlarmApp()
{
	while (!mCommandProcesses.isEmpty())
	{
		ProcData* pd = mCommandProcesses[0];
		mCommandProcesses.pop_front();
		delete pd;
	}
	AlarmCalendar::terminateCalendars();
}

/******************************************************************************
* Return the one and only KAlarmApp instance.
* If it doesn't already exist, it is created first.
*/
KAlarmApp* KAlarmApp::getInstance()
{
	if (!theInstance)
	{
		theInstance = new KAlarmApp;

		if (mFatalError)
			theInstance->quitFatal();
		else
		{
			// This is here instead of in the constructor to avoid recursion
			Daemon::initialise();    // calendars must be initialised before calling this
			Daemon::connectRegistered(AlarmCalendar::resources(), SLOT(slotDaemonRegistered(bool)));
		}
	}
	return theInstance;
}

/******************************************************************************
* Restore the saved session if required.
*/
bool KAlarmApp::restoreSession()
{
	if (!isSessionRestored())
		return false;
	if (mFatalError)
	{
		quitFatal();
		return false;
	}

	// Process is being restored by session management.
	kDebug(5950) << "KAlarmApp::restoreSession(): Restoring\n";
	++mActiveCount;
	if (!initCheck(true))     // open the calendar file (needed for main windows)
	{
		--mActiveCount;
		quitIf(1, true);    // error opening the main calendar - quit
		return true;
	}
	MainWindow* trayParent = 0;
	for (int i = 1;  KMainWindow::canBeRestored(i);  ++i)
	{
		QString type = KMainWindow::classNameOfToplevel(i);
		if (type == QLatin1String("MainWindow"))
		{
			MainWindow* win = MainWindow::create(true);
			win->restore(i, false);
			if (win->isHiddenTrayParent())
				trayParent = win;
			else
				win->show();
		}
		else if (type == QLatin1String("MessageWin"))
		{
			MessageWin* win = new MessageWin;
			win->restore(i, false);
			if (win->isValid())
				win->show();
			else
				delete win;
		}
	}
	initCheck();           // register with the alarm daemon

	// Try to display the system tray icon if it is configured to be autostarted,
	// or if we're in run-in-system-tray mode.
	if (Preferences::autostartTrayIcon()
	||  MainWindow::count()  &&  wantRunInSystemTray())
	{
		displayTrayIcon(true, trayParent);
		// Occasionally for no obvious reason, the main main window is
		// shown when it should be hidden, so hide it just to be sure.
		if (trayParent)
			trayParent->hide();
	}

	--mActiveCount;
	quitIf(0);           // quit if no windows are open
	return true;
}

/******************************************************************************
* Called for a KUniqueApplication when a new instance of the application is
* started.
*/
int KAlarmApp::newInstance()
{
	kDebug(5950)<<"KAlarmApp::newInstance()\n";
	if (mFatalError)
	{
		quitFatal();
		return 1;
	}
	++mActiveCount;
	int exitCode = 0;               // default = success
	static bool firstInstance = true;
	bool dontRedisplay = false;
	if (!firstInstance || !isSessionRestored())
	{
		QString usage;
		KCmdLineArgs* args = KCmdLineArgs::parsedArgs();

		// Use a 'do' loop which is executed only once to allow easy error exits.
		// Errors use 'break' to skip to the end of the function.

		// Note that DCOP handling is only set up once the command line parameters
		// have been checked, since we mustn't register with the alarm daemon only
		// to quit immediately afterwards.
		do
		{
			#define USAGE(message)  { usage = message; break; }
			if (args->isSet("stop"))
			{
				// Stop the alarm daemon
				kDebug(5950)<<"KAlarmApp::newInstance(): stop\n";
				args->clear();         // free up memory
				if (!Daemon::stop())
				{
					exitCode = 1;
					break;
				}
				dontRedisplay = true;  // exit program if no other instances running
			}
			else
			if (args->isSet("reset"))
			{
				// Reset the alarm daemon, if it's running.
				// (If it's not running, it will reset automatically when it eventually starts.)
				kDebug(5950)<<"KAlarmApp::newInstance(): reset\n";
				args->clear();         // free up memory
				Daemon::reset();
				dontRedisplay = true;  // exit program if no other instances running
			}
			else
			if (args->isSet("tray"))
			{
				// Display only the system tray icon
				kDebug(5950)<<"KAlarmApp::newInstance(): tray\n";
				args->clear();      // free up memory
				if (!mHaveSystemTray)
				{
					exitCode = 1;
					break;
				}
				if (!initCheck())   // open the calendar, register with daemon
				{
					exitCode = 1;
					break;
				}
				if (!displayTrayIcon(true))
				{
					exitCode = 1;
					break;
				}
			}
			else
			if (args->isSet("handleEvent")  ||  args->isSet("triggerEvent")  ||  args->isSet("cancelEvent"))
			{
				// Display or delete the event with the specified event ID
				kDebug(5950)<<"KAlarmApp::newInstance(): handle event\n";
				EventFunc function = EVENT_HANDLE;
				int count = 0;
				const char* option = 0;
				if (args->isSet("handleEvent"))   { function = EVENT_HANDLE;   option = "handleEvent";   ++count; }
				if (args->isSet("triggerEvent"))  { function = EVENT_TRIGGER;  option = "triggerEvent";  ++count; }
				if (args->isSet("cancelEvent"))   { function = EVENT_CANCEL;   option = "cancelEvent";   ++count; }
				if (count > 1)
					USAGE(i18n("%1, %2, %3 mutually exclusive", QLatin1String("--handleEvent"), QLatin1String("--triggerEvent"), QLatin1String("--cancelEvent")));
				if (!initCheck(true))   // open the calendar, don't register with daemon yet
				{
					exitCode = 1;
					break;
				}
				QString eventID = args->getOption(option);
				args->clear();      // free up memory
				if (eventID.startsWith(QLatin1String("ad:")))
				{
					// It's a notification from the alarm deamon
					eventID = eventID.mid(3);
					Daemon::queueEvent(eventID);
				}
				setUpDcop();        // start processing DCOP calls
				if (!handleEvent(eventID, function))
				{
					exitCode = 1;
					break;
				}
			}
			else
			if (args->isSet("edit"))
			{
				QString eventID = args->getOption("edit");
				if (!initCheck())
				{
					exitCode = 1;
					break;
				}
				if (!KAlarm::edit(eventID))
				{
					USAGE(i18n("%1: Event %2 not found, or not editable", QString::fromLatin1("--edit"), eventID))
					exitCode = 1;
					break;
				}
			}
			else
			if (args->isSet("edit-new")  ||  args->isSet("edit-new-preset"))
			{
				QString templ;
				if (args->isSet("edit-new-preset"))
					templ = args->getOption("edit-new-preset");
				if (!initCheck())
				{
					exitCode = 1;
					break;
				}
				KAlarm::editNew(templ);
			}
			else
			if (args->isSet("file")  ||  args->isSet("exec")  ||  args->isSet("mail")  ||  args->count())
			{
				// Display a message or file, execute a command, or send an email
				KAEvent::Action action = KAEvent::MESSAGE;
				QByteArray       alMessage;
				QByteArray       alFromID;
				EmailAddressList alAddresses;
				QStringList      alAttachments;
				QByteArray       alSubject;
				if (args->isSet("file"))
				{
					kDebug(5950)<<"KAlarmApp::newInstance(): file\n";
					if (args->isSet("exec"))
						USAGE(i18n("%1 incompatible with %2", QLatin1String("--exec"), QLatin1String("--file")))
					if (args->isSet("mail"))
						USAGE(i18n("%1 incompatible with %2", QLatin1String("--mail"), QLatin1String("--file")))
					if (args->count())
						USAGE(i18n("message incompatible with %1", QLatin1String("--file")))
					alMessage = args->getOption("file");
					action = KAEvent::FILE;
				}
				else if (args->isSet("exec"))
				{
					kDebug(5950)<<"KAlarmApp::newInstance(): exec\n";
					if (args->isSet("mail"))
						USAGE(i18n("%1 incompatible with %2", QLatin1String("--mail"), QLatin1String("--exec")))
					alMessage = args->getOption("exec");
					int n = args->count();
					for (int i = 0;  i < n;  ++i)
					{
						alMessage += ' ';
						alMessage += args->arg(i);
					}
					action = KAEvent::COMMAND;
				}
				else if (args->isSet("mail"))
				{
					kDebug(5950)<<"KAlarmApp::newInstance(): mail\n";
					if (args->isSet("subject"))
						alSubject = args->getOption("subject");
					if (args->isSet("from-id"))
						alFromID = args->getOption("from-id");
					QByteArrayList params = args->getOptionList("mail");
					for (QByteArrayList::Iterator i = params.begin();  i != params.end();  ++i)
					{
						QString addr = QString::fromLocal8Bit(*i);
						if (!KAMail::checkAddress(addr))
							USAGE(i18n("%1: invalid email address", QLatin1String("--mail")))
						alAddresses += KCal::Person(QString(), addr);
					}
					params = args->getOptionList("attach");
					for (QByteArrayList::Iterator i = params.begin();  i != params.end();  ++i)
						alAttachments += QString::fromLocal8Bit(*i);
					alMessage = args->arg(0);
					action = KAEvent::EMAIL;
				}
				else
				{
					kDebug(5950)<<"KAlarmApp::newInstance(): message\n";
					alMessage = args->arg(0);
				}

				if (action != KAEvent::EMAIL)
				{
					if (args->isSet("subject"))
						USAGE(i18n("%1 requires %2", QLatin1String("--subject"), QLatin1String("--mail")))
					if (args->isSet("from-id"))
						USAGE(i18n("%1 requires %2", QLatin1String("--from-id"), QLatin1String("--mail")))
					if (args->isSet("attach"))
						USAGE(i18n("%1 requires %2", QLatin1String("--attach"), QLatin1String("--mail")))
					if (args->isSet("bcc"))
						USAGE(i18n("%1 requires %2", QLatin1String("--bcc"), QLatin1String("--mail")))
				}

				KDateTime alarmTime, endTime;
				QColor    bgColour = Preferences::defaultBgColour();
				QColor    fgColour = Preferences::defaultFgColour();
				KARecurrence recurrence;
				int       repeatCount    = 0;
				int       repeatInterval = 0;
				if (args->isSet("color"))
				{
					// Background colour is specified
					QByteArray colourText = args->getOption("color");
					if (static_cast<const char*>(colourText)[0] == '0'
					&&  tolower(static_cast<const char*>(colourText)[1]) == 'x')
						colourText.replace(0, 2, "#");
					bgColour.setNamedColor(colourText);
					if (!bgColour.isValid())
						USAGE(i18n("Invalid %1 parameter", QLatin1String("--color")))
				}
				if (args->isSet("colorfg"))
				{
					// Foreground colour is specified
					QByteArray colourText = args->getOption("colorfg");
					if (static_cast<const char*>(colourText)[0] == '0'
					&&  tolower(static_cast<const char*>(colourText)[1]) == 'x')
						colourText.replace(0, 2, "#");
					fgColour.setNamedColor(colourText);
					if (!fgColour.isValid())
						USAGE(i18n("Invalid %1 parameter", QLatin1String("--colorfg")))
				}

				if (args->isSet("time"))
				{
					QByteArray dateTime = args->getOption("time");
					if (!convWakeTime(dateTime, alarmTime))
						USAGE(i18n("Invalid %1 parameter", QLatin1String("--time")))
				}
				else
					alarmTime = KDateTime::currentLocalDateTime();

				bool haveRecurrence = args->isSet("recurrence");
				if (haveRecurrence)
				{
					if (args->isSet("login"))
						USAGE(i18n("%1 incompatible with %2", QLatin1String("--login"), QLatin1String("--recurrence")))
					if (args->isSet("until"))
						USAGE(i18n("%1 incompatible with %2", QLatin1String("--until"), QLatin1String("--recurrence")))
					QByteArray rule = args->getOption("recurrence");
					recurrence.set(QString::fromLocal8Bit(static_cast<const char*>(rule)));
				}
				if (args->isSet("interval"))
				{
					// Repeat count is specified
					int count;
					if (args->isSet("login"))
						USAGE(i18n("%1 incompatible with %2", QLatin1String("--login"), QLatin1String("--interval")))
					bool ok;
					if (args->isSet("repeat"))
					{
						count = args->getOption("repeat").toInt(&ok);
						if (!ok || !count || count < -1 || (count < 0 && haveRecurrence))
							USAGE(i18n("Invalid %1 parameter", QLatin1String("--repeat")))
					}
					else if (haveRecurrence)
						USAGE(i18n("%1 requires %2", QLatin1String("--interval"), QLatin1String("--repeat")))
					else if (args->isSet("until"))
					{
						count = 0;
						QByteArray dateTime = args->getOption("until");
						bool ok;
						if (args->isSet("time"))
							ok = convWakeTime(dateTime, endTime, alarmTime);
						else
							ok = convWakeTime(dateTime, endTime);
						if (!ok)
							USAGE(i18n("Invalid %1 parameter", QLatin1String("--until")))
						if (alarmTime.isDateOnly()  &&  !endTime.isDateOnly())
							USAGE(i18n("Invalid %1 parameter for date-only alarm", QLatin1String("--until")))
						if (!alarmTime.isDateOnly()  &&  endTime.isDateOnly())
							endTime.setTime(QTime(23,59,59));
						if (endTime < alarmTime)
							USAGE(i18n("%1 earlier than %2", QLatin1String("--until"), QLatin1String("--time")))
					}
					else
						count = -1;

					// Get the recurrence interval
					int interval;
					KARecurrence::Type recurType;
					if (!convInterval(args->getOption("interval"), recurType, interval, !haveRecurrence)
					||  interval < 0)
						USAGE(i18n("Invalid %1 parameter", QLatin1String("--interval")))
					if (alarmTime.isDateOnly()  &&  recurType == KARecurrence::MINUTELY)
						USAGE(i18n("Invalid %1 parameter for date-only alarm", QLatin1String("--interval")))

					if (haveRecurrence)
					{
						// There is a also a recurrence specified, so set up a simple repetition
						int longestInterval = recurrence.longestInterval();
						if (count * interval > longestInterval)
							USAGE(i18n("Invalid %1 and %2 parameters: repetition is longer than %3 interval", QLatin1String("--interval"), QLatin1String("--repeat"), QLatin1String("--recurrence")));
						repeatCount    = count;
						repeatInterval = interval;
					}
					else
					{
						// There is no other recurrence specified, so convert the repetition
						// parameters into a KCal::Recurrence
						recurrence.set(recurType, interval, count, alarmTime, endTime);
					}
				}
				else
				{
					if (args->isSet("repeat"))
						USAGE(i18n("%1 requires %2", QLatin1String("--repeat"), QLatin1String("--interval")))
					if (args->isSet("until"))
						USAGE(i18n("%1 requires %2", QLatin1String("--until"), QLatin1String("--interval")))
				}

				QByteArray audioFile;
				float      audioVolume = -1;
				bool       audioRepeat = args->isSet("play-repeat");
				if (audioRepeat  ||  args->isSet("play"))
				{
					// Play a sound with the alarm
					if (audioRepeat  &&  args->isSet("play"))
						USAGE(i18n("%1 incompatible with %2", QLatin1String("--play"), QLatin1String("--play-repeat")))
					if (args->isSet("beep"))
						USAGE(i18n("%1 incompatible with %2", QLatin1String("--beep"), QLatin1String(audioRepeat ? "--play-repeat" : "--play")))
					if (args->isSet("speak"))
						USAGE(i18n("%1 incompatible with %2", QLatin1String("--speak"), QLatin1String(audioRepeat ? "--play-repeat" : "--play")))
					audioFile = args->getOption(audioRepeat ? "play-repeat" : "play");
					if (args->isSet("volume"))
					{
						bool ok;
						int volumepc = args->getOption("volume").toInt(&ok);
						if (!ok  ||  volumepc < 0  ||  volumepc > 100)
							USAGE(i18n("Invalid %1 parameter", QLatin1String("--volume")))
						audioVolume = static_cast<float>(volumepc) / 100;
					}
				}
				else if (args->isSet("volume"))
					USAGE(i18n("%1 requires %2 or %3", QLatin1String("--volume"), QLatin1String("--play"), QLatin1String("--play-repeat")))
				if (args->isSet("speak"))
				{
					if (args->isSet("beep"))
						USAGE(i18n("%1 incompatible with %2", QLatin1String("--beep"), QLatin1String("--speak")))
					if (!mSpeechEnabled)
						USAGE(i18n("%1 requires speech synthesis to be configured using KTTSD", QLatin1String("--speak")))
				}
				int reminderMinutes = 0;
				bool onceOnly = args->isSet("reminder-once");
				if (args->isSet("reminder")  ||  onceOnly)
				{
					// Issue a reminder alarm in advance of the main alarm
					if (onceOnly  &&  args->isSet("reminder"))
						USAGE(i18n("%1 incompatible with %2", QLatin1String("--reminder"), QLatin1String("--reminder-once")))
					QString opt = onceOnly ? QLatin1String("--reminder-once") : QLatin1String("--reminder");
					if (args->isSet("exec"))
						USAGE(i18n("%1 incompatible with %2", opt, QLatin1String("--exec")))
					if (args->isSet("mail"))
						USAGE(i18n("%1 incompatible with %2", opt, QLatin1String("--mail")))
					KARecurrence::Type recurType;
					QString optval = args->getOption(onceOnly ? "reminder-once" : "reminder");
					bool ok = convInterval(args->getOption(onceOnly ? "reminder-once" : "reminder"), recurType, reminderMinutes);
					if (ok)
					{
						switch (recurType)
						{
							case KARecurrence::MINUTELY:
								if (alarmTime.isDateOnly())
									USAGE(i18n("Invalid %1 parameter for date-only alarm", opt))
								break;
							case KARecurrence::DAILY:     reminderMinutes *= 1440;  break;
							case KARecurrence::WEEKLY:    reminderMinutes *= 7*1440;  break;
							default:   ok = false;  break;
						}
					}
					if (!ok)
						USAGE(i18n("Invalid %1 parameter", opt))
				}

				int lateCancel = 0;
				if (args->isSet("late-cancel"))
				{
					KARecurrence::Type recurType;
					bool ok = convInterval(args->getOption("late-cancel"), recurType, lateCancel, false);
					if (!ok  ||  lateCancel <= 0)
						USAGE(i18n("Invalid %1 parameter", QLatin1String("late-cancel")))
				}
				else if (args->isSet("auto-close"))
					USAGE(i18n("%1 requires %2", QLatin1String("--auto-close"), QLatin1String("--late-cancel")))

				int flags = KAEvent::DEFAULT_FONT;
				if (args->isSet("ack-confirm"))
					flags |= KAEvent::CONFIRM_ACK;
				if (args->isSet("auto-close"))
					flags |= KAEvent::AUTO_CLOSE;
				if (args->isSet("beep"))
					flags |= KAEvent::BEEP;
				if (args->isSet("speak"))
					flags |= KAEvent::SPEAK;
				if (args->isSet("korganizer"))
					flags |= KAEvent::COPY_KORGANIZER;
				if (args->isSet("disable"))
					flags |= KAEvent::DISABLED;
				if (audioRepeat)
					flags |= KAEvent::REPEAT_SOUND;
				if (args->isSet("login"))
					flags |= KAEvent::REPEAT_AT_LOGIN;
				if (args->isSet("bcc"))
					flags |= KAEvent::EMAIL_BCC;
				if (alarmTime.isDateOnly())
					flags |= KAEvent::ANY_TIME;
				args->clear();      // free up memory

				// Display or schedule the event
				if (!initCheck())
				{
					exitCode = 1;
					break;
				}
				if (!scheduleEvent(action, alMessage, alarmTime, lateCancel, flags, bgColour, fgColour, QFont(), audioFile,
				                   audioVolume, reminderMinutes, recurrence, repeatInterval, repeatCount,
				                   alFromID, alAddresses, alSubject, alAttachments))
				{
					exitCode = 1;
					break;
				}
			}
			else
			{
				// No arguments - run interactively & display the main window
				kDebug(5950)<<"KAlarmApp::newInstance(): interactive\n";
				if (args->isSet("ack-confirm"))
					usage += QLatin1String("--ack-confirm ");
				if (args->isSet("attach"))
					usage += QLatin1String("--attach ");
				if (args->isSet("auto-close"))
					usage += QLatin1String("--auto-close ");
				if (args->isSet("bcc"))
					usage += QLatin1String("--bcc ");
				if (args->isSet("beep"))
					usage += QLatin1String("--beep ");
				if (args->isSet("color"))
					usage += QLatin1String("--color ");
				if (args->isSet("colorfg"))
					usage += QLatin1String("--colorfg ");
				if (args->isSet("disable"))
					usage += QLatin1String("--disable ");
				if (args->isSet("from-id"))
					usage += QLatin1String("--from-id ");
				if (args->isSet("korganizer"))
					usage += QLatin1String("--korganizer ");
				if (args->isSet("late-cancel"))
					usage += QLatin1String("--late-cancel ");
				if (args->isSet("login"))
					usage += QLatin1String("--login ");
				if (args->isSet("play"))
					usage += QLatin1String("--play ");
				if (args->isSet("play-repeat"))
					usage += QLatin1String("--play-repeat ");
				if (args->isSet("reminder"))
					usage += QLatin1String("--reminder ");
				if (args->isSet("reminder-once"))
					usage += QLatin1String("--reminder-once ");
				if (args->isSet("speak"))
					usage += QLatin1String("--speak ");
				if (args->isSet("subject"))
					usage += QLatin1String("--subject ");
				if (args->isSet("time"))
					usage += QLatin1String("--time ");
				if (args->isSet("volume"))
					usage += QLatin1String("--volume ");
				if (!usage.isEmpty())
				{
					usage += i18n(": option(s) only valid with a message/%1/%2", QLatin1String("--file"), QLatin1String("--exec"));
					break;
				}

				args->clear();      // free up memory
				if (!initCheck())
				{
					exitCode = 1;
					break;
				}

				(MainWindow::create())->show();
			}
		} while (0);    // only execute once

		if (!usage.isEmpty())
		{
			// Note: we can't use args->usage() since that also quits any other
			// running 'instances' of the program.
			std::cerr << usage.toLocal8Bit().data()
			          << i18n("\nUse --help to get a list of available command line options.\n").toLocal8Bit().data();
			exitCode = 1;
		}
	}
	if (firstInstance  &&  !dontRedisplay  &&  !exitCode)
		MessageWin::redisplayAlarms();

	--mActiveCount;
	firstInstance = false;

	// Quit the application if this was the last/only running "instance" of the program.
	// Executing 'return' doesn't work very well since the program continues to
	// run if no windows were created.
	quitIf(exitCode);
	return exitCode;
}

/******************************************************************************
* Quit the program, optionally only if there are no more "instances" running.
*/
void KAlarmApp::quitIf(int exitCode, bool force)
{
	if (force)
	{
		// Quit regardless, except for message windows
		MainWindow::closeAll();
		displayTrayIcon(false);
		if (MessageWin::instanceCount())
			return;
	}
	else
	{
		// Quit only if there are no more "instances" running
		mPendingQuit = false;
		if (mActiveCount > 0  ||  MessageWin::instanceCount())
			return;
		int mwcount = MainWindow::count();
		MainWindow* mw = mwcount ? MainWindow::firstWindow() : 0;
		if (mwcount > 1  ||  mwcount && (!mw->isHidden() || !mw->isTrayParent()))
			return;
		// There are no windows left except perhaps a main window which is a hidden tray icon parent
		if (mTrayWindow)
		{
			// There is a system tray icon.
			// Don't exit unless the system tray doesn't seem to exist.
			if (checkSystemTray())
				return;
		}
		if (!mDcopQueue.isEmpty()  ||  !mCommandProcesses.isEmpty())
		{
			// Don't quit yet if there are outstanding actions on the DCOP queue
			mPendingQuit = true;
			mPendingQuitCode = exitCode;
			return;
		}
	}

	// This was the last/only running "instance" of the program, so exit completely.
	kDebug(5950) << "KAlarmApp::quitIf(" << exitCode << "): quitting" << endl;
	exit(exitCode);
}

/******************************************************************************
* Called when the Quit menu item is selected.
* Closes the system tray window and all main windows, but does not exit the
* program if other windows are still open.
*/
void KAlarmApp::doQuit(QWidget* parent)
{
	kDebug(5950) << "KAlarmApp::doQuit()\n";
	if (mDisableAlarmsIfStopped
	&&  MessageBox::warningContinueCancel(parent, KMessageBox::Cancel,
	                                      i18n("Quitting will disable alarms\n(once any alarm message windows are closed)."),
	                                      QString(), KStdGuiItem::quit(), Preferences::QUIT_WARN
	                                     ) != KMessageBox::Yes)
		return;
	quitIf(0, true);
}

/******************************************************************************
* Called when the session manager is about to close down the application.
*/
void KAlarmApp::commitData(QSessionManager& sm)
{
	mSessionClosingDown = true;
	KUniqueApplication::commitData(sm);
	mSessionClosingDown = false;         // reset in case shutdown is cancelled
}

/******************************************************************************
* Display an error message for a fatal error. Prevent further actions since
* the program state is unsafe.
*/
void KAlarmApp::displayFatalError(const QString& message)
{
	if (!mFatalError)
	{
		mFatalError = 1;
		mFatalMessage = message;
		if (theInstance)
			QTimer::singleShot(0, theInstance, SLOT(quitFatal()));
	}
}

/******************************************************************************
* Quit the program, once the fatal error message has been acknowledged.
*/
void KAlarmApp::quitFatal()
{
	switch (mFatalError)
	{
		case 0:
		case 2:
			return;
		case 1:
			mFatalError = 2;
			KMessageBox::error(0, mFatalMessage);
			mFatalError = 3;
			// fall through to '3'
		case 3:
			if (theInstance)
				theInstance->quitIf(1, true);
			break;
	}
	QTimer::singleShot(1000, this, SLOT(quitFatal()));
}

/******************************************************************************
* The main processing loop for KAlarm.
* All KAlarm operations involving opening or updating calendar files are called
* from this loop to ensure that only one operation is active at any one time.
* This precaution is necessary because KAlarm's activities are mostly
* asynchronous, being in response to DCOP calls from the alarm daemon (or other
* programs) or timer events, any of which can be received in the middle of
* performing another operation. If a calendar file is opened or updated while
* another calendar operation is in progress, the program has been observed to
* hang, or the first calendar call has failed with data loss - clearly
* unacceptable!!
*/
void KAlarmApp::processQueue()
{
	if (mInitialised  &&  !mProcessingQueue)
	{
		kDebug(5950) << "KAlarmApp::processQueue()\n";
		mProcessingQueue = true;

		// Reset the alarm daemon if it's been queued
		KAlarm::resetDaemonIfQueued();

		// Process DCOP calls
		while (!mDcopQueue.isEmpty())
		{
			DcopQEntry& entry = mDcopQueue.head();
			if (entry.eventId.isEmpty())
			{
				// It's a new alarm
				switch (entry.function)
				{
				case EVENT_TRIGGER:
					execAlarm(entry.event, entry.event.firstAlarm(), false);
					break;
				case EVENT_HANDLE:
					KAlarm::addEvent(entry.event, 0, 0, 0, KAlarm::ALLOW_KORG_UPDATE | KAlarm::NO_RESOURCE_PROMPT);
					break;
				case EVENT_CANCEL:
					break;
				}
			}
			else
				handleEvent(entry.eventId, entry.function);
			mDcopQueue.dequeue();
		}

		// Purge the archived alarms resources if it's time to do so
		AlarmCalendar::resources()->purgeIfQueued();

		// Now that the queue has been processed, quit if a quit was queued
		if (mPendingQuit)
			quitIf(mPendingQuitCode);

		mProcessingQueue = false;
	}
}

/******************************************************************************
* Called when the system tray main window is closed.
*/
void KAlarmApp::removeWindow(TrayWindow*)
{
	mTrayWindow = 0;
	quitIf();
}

/******************************************************************************
*  Display or close the system tray icon.
*/
bool KAlarmApp::displayTrayIcon(bool show, MainWindow* parent)
{
	static bool creating = false;
	if (show)
	{
		if (!mTrayWindow  &&  !creating)
		{
			if (!mHaveSystemTray)
				return false;
			if (!MainWindow::count()  &&  wantRunInSystemTray())
			{
				creating = true;    // prevent main window constructor from creating an additional tray icon
				parent = MainWindow::create();
				creating = false;
			}
			mTrayWindow = new TrayWindow(parent ? parent : MainWindow::firstWindow());
			connect(mTrayWindow, SIGNAL(deleted()), SIGNAL(trayIconToggled()));
			mTrayWindow->show();
			emit trayIconToggled();

			// Set up a timer so that we can check after all events in the window system's
			// event queue have been processed, whether the system tray actually exists
			mCheckingSystemTray = true;
			mSavedNoSystemTray  = mNoSystemTray;
			mNoSystemTray       = false;
			QTimer::singleShot(0, this, SLOT(slotSystemTrayTimer()));
		}
	}
	else if (mTrayWindow)
	{
		delete mTrayWindow;
		mTrayWindow = 0;
	}
	return true;
}

/******************************************************************************
*  Called by a timer to check whether the system tray icon has been housed in
*  the system tray. Because there is a delay between the system tray icon show
*  event and the icon being reparented by the system tray, we have to use a
*  timer to check whether the system tray has actually grabbed it, or whether
*  the system tray probably doesn't exist.
*/
void KAlarmApp::slotSystemTrayTimer()
{
	mCheckingSystemTray = false;
	if (!checkSystemTray())
		quitIf(0);    // exit the application if there are no open windows
}

/******************************************************************************
*  Check whether the system tray icon has been housed in the system tray.
*  If the system tray doesn't seem to exist, tell the alarm daemon to notify us
*  of alarms regardless of whether we're running.
*/
bool KAlarmApp::checkSystemTray()
{
	if (mCheckingSystemTray  ||  !mTrayWindow)
		return true;
	if (mTrayWindow->inSystemTray() != !mSavedNoSystemTray)
	{
		kDebug(5950) << "KAlarmApp::checkSystemTray(): changed -> " << mSavedNoSystemTray << endl;
		mNoSystemTray = mSavedNoSystemTray = !mSavedNoSystemTray;

		// Store the new setting in the config file, so that if KAlarm exits and is then
		// next activated by the daemon to display a message, it will register with the
		// daemon with the correct NOTIFY type. If that happened when there was no system
		// tray and alarms are disabled when KAlarm is not running, registering with
		// NO_START_NOTIFY could result in alarms never being seen.
		KConfig* config = KGlobal::config();
		config->setGroup(QLatin1String("General"));
		config->writeEntry(QLatin1String("NoSystemTray"), mNoSystemTray);
		config->sync();

		// Update other settings and reregister with the alarm daemon
		slotPreferencesChanged();
	}
	else
	{
		kDebug(5950) << "KAlarmApp::checkSystemTray(): no change = " << !mSavedNoSystemTray << endl;
		mNoSystemTray = mSavedNoSystemTray;
	}
	return !mNoSystemTray;
}

/******************************************************************************
* Return the main window associated with the system tray icon.
*/
MainWindow* KAlarmApp::trayMainWindow() const
{
	return mTrayWindow ? mTrayWindow->assocMainWindow() : 0;
}

/******************************************************************************
*  Called when KAlarm preferences have changed.
*/
void KAlarmApp::slotPreferencesChanged()
{
	bool newRunInSysTray = wantRunInSystemTray();
	if (newRunInSysTray != mOldRunInSystemTray)
	{
		// The system tray run mode has changed
		++mActiveCount;         // prevent the application from quitting
		MainWindow* win = mTrayWindow ? mTrayWindow->assocMainWindow() : 0;
		delete mTrayWindow;     // remove the system tray icon if it is currently shown
		mTrayWindow = 0;
		mOldRunInSystemTray = newRunInSysTray;
		if (!newRunInSysTray)
		{
			if (win  &&  win->isHidden())
				delete win;
		}
		displayTrayIcon(true);
		--mActiveCount;
	}

	bool newDisableIfStopped = wantRunInSystemTray() && !mNoSystemTray && Preferences::disableAlarmsIfStopped();
	if (newDisableIfStopped != mDisableAlarmsIfStopped)
	{
		mDisableAlarmsIfStopped = newDisableIfStopped;    // N.B. this setting is used by Daemon::reregister()
		Preferences::setQuitWarn(true);   // since mode has changed, re-allow warning messages on Quit
		Daemon::reregister();           // re-register with the alarm daemon
	}

	// Change alarm times for date-only alarms if the start of day time has changed
	if (Preferences::startOfDay() != mStartOfDay)
		changeStartOfDay();

	// In case the date for February 29th recurrences has changed
	KARecurrence::setDefaultFeb29Type(Preferences::defaultFeb29Type());

	if (Preferences::showAlarmTime()   != mPrefsShowTime
	||  Preferences::showTimeToAlarm() != mPrefsShowTimeTo)
	{
		// The default alarm list time columns selection has changed
		MainWindow::updateTimeColumns(mPrefsShowTime, mPrefsShowTimeTo);
		mPrefsShowTime   = Preferences::showAlarmTime();
		mPrefsShowTimeTo = Preferences::showTimeToAlarm();
	}

	if (Preferences::archivedColour() != mPrefsArchivedColour)
	{
		// The archived alarms text colour has changed
		mRefreshArchivedAlarms = true;
		mPrefsArchivedColour = Preferences::archivedColour();
	}

	if (Preferences::archivedKeepDays() != mPrefsArchivedKeepDays)
	{
		// How long archived alarms are being kept has changed.
		// N.B. This also adjusts for any change in start-of-day time.
		mPrefsArchivedKeepDays = Preferences::archivedKeepDays();
		AlarmCalendar::resources()->setPurgeDays(mPrefsArchivedKeepDays);
	}

	if (mRefreshArchivedAlarms)
	{
		mRefreshArchivedAlarms = false;
		MainWindow::updateArchived();
	}
}

/******************************************************************************
*  Change alarm times for date-only alarms after the start of day time has changed.
*/
void KAlarmApp::changeStartOfDay()
{
	QTime sod = Preferences::startOfDay();
	DateTime::setStartOfDay(sod);
	AlarmCalendar* cal = AlarmCalendar::resources();
	if (KAEvent::adjustStartOfDay(cal->events(KCalEvent::ACTIVE)))
		cal->save();
	Preferences::updateStartOfDayCheck();  // now that calendar is updated, set OK flag in config file
	mStartOfDay = sod;
}

/******************************************************************************
*  Called when the archived alarms resources have been purged.
*  Updates the alarm list in all main windows.
*/
void KAlarmApp::slotArchivedPurged()
{
	mRefreshArchivedAlarms = false;
	MainWindow::updateArchived();
}

/******************************************************************************
*  Return whether the program is configured to be running in the system tray.
*/
bool KAlarmApp::wantRunInSystemTray() const
{
	return Preferences::runInSystemTray()  &&  mHaveSystemTray;
}

/******************************************************************************
* Called to schedule a new alarm, either in response to a DCOP notification or
* to command line options.
* Reply = true unless there was a parameter error or an error opening calendar file.
*/
bool KAlarmApp::scheduleEvent(KAEvent::Action action, const QString& text, const KDateTime& dateTime,
                              int lateCancel, int flags, const QColor& bg, const QColor& fg, const QFont& font,
                              const QString& audioFile, float audioVolume, int reminderMinutes,
                              const KARecurrence& recurrence, int repeatInterval, int repeatCount,
                              const QString& mailFromID, const EmailAddressList& mailAddresses,
                              const QString& mailSubject, const QStringList& mailAttachments)
{
	kDebug(5950) << "KAlarmApp::scheduleEvent(): " << text << endl;
	if (!dateTime.isValid())
		return false;
	KDateTime now = KDateTime::currentUtcDateTime();
	if (lateCancel  &&  dateTime < now.addSecs(-maxLateness(lateCancel)))
		return true;               // alarm time was already archived too long ago
	KDateTime alarmTime = dateTime;
	// Round down to the nearest minute to avoid scheduling being messed up
	if (!dateTime.isDateOnly())
		alarmTime.setTime(QTime(alarmTime.time().hour(), alarmTime.time().minute(), 0));

	KAEvent event(alarmTime, text, bg, fg, font, action, lateCancel, flags);
	if (reminderMinutes)
	{
		bool onceOnly = (reminderMinutes < 0);
		event.setReminder((onceOnly ? -reminderMinutes : reminderMinutes), onceOnly);
	}
	if (!audioFile.isEmpty())
		event.setAudioFile(audioFile, audioVolume, -1, 0);
	if (!mailAddresses.isEmpty())
		event.setEmail(mailFromID, mailAddresses, mailSubject, mailAttachments);
	event.setRecurrence(recurrence);
	event.setFirstRecurrence();
	event.setRepetition(repeatInterval, repeatCount - 1);
	if (alarmTime <= now)
	{
		// Alarm is due for display already.
		// First execute it once without adding it to the calendar file.
		if (!mInitialised)
			mDcopQueue.enqueue(DcopQEntry(event, EVENT_TRIGGER));
		else
			execAlarm(event, event.firstAlarm(), false);
		// If it's a recurring alarm, reschedule it for its next occurrence
		if (!event.recurs()
		||  event.setNextOccurrence(now, true) == KAEvent::NO_OCCURRENCE)
			return true;
		// It has recurrences in the future
	}

	// Queue the alarm for insertion into the calendar file
	mDcopQueue.enqueue(DcopQEntry(event));
	if (mInitialised)
		QTimer::singleShot(0, this, SLOT(processQueue()));
	return true;
}

/******************************************************************************
* Called in response to a DCOP notification by the alarm daemon that an event
* should be handled, i.e. displayed or cancelled.
* Optionally display the event. Delete the event from the calendar file and
* from every main window instance.
*/
bool KAlarmApp::dcopHandleEvent(const QString& eventID, EventFunc function)
{
	kDebug(5950) << "KAlarmApp::dcopHandleEvent(" << eventID << ")\n";
	mDcopQueue.append(DcopQEntry(function, eventID));
	if (mInitialised)
		QTimer::singleShot(0, this, SLOT(processQueue()));
	return true;
}

/******************************************************************************
* Either:
* a) Display the event and then delete it if it has no outstanding repetitions.
* b) Delete the event.
* c) Reschedule the event for its next repetition. If none remain, delete it.
* If the event is deleted, it is removed from the calendar file and from every
* main window instance.
*/
bool KAlarmApp::handleEvent(const QString& eventID, EventFunc function)
{
	kDebug(5950) << "KAlarmApp::handleEvent(): " << eventID << ", " << (function==EVENT_TRIGGER?"TRIGGER":function==EVENT_CANCEL?"CANCEL":function==EVENT_HANDLE?"HANDLE":"?") << endl;
	KCal::Event* kcalEvent = AlarmCalendar::resources()->event(eventID);
	if (!kcalEvent)
	{
		kWarning(5950) << "KAlarmApp::handleEvent(): event ID not found: " << eventID << endl;
		Daemon::eventHandled(eventID);
		return false;
	}
	KAEvent event(kcalEvent);
	switch (function)
	{
		case EVENT_CANCEL:
			KAlarm::deleteEvent(event, true);
			break;

		case EVENT_TRIGGER:    // handle it if it's due, else execute it regardless
		case EVENT_HANDLE:     // handle it if it's due
		{
			KDateTime now = KDateTime::currentUtcDateTime();
			DateTime  repeatDT;
			bool updateCalAndDisplay = false;
			bool alarmToExecuteValid = false;
			KAAlarm alarmToExecute;
			// Check all the alarms in turn.
			// Note that the main alarm is fetched before any other alarms.
			for (KAAlarm alarm = event.firstAlarm();  alarm.valid();  alarm = event.nextAlarm(alarm))
			{
				if (alarm.deferred()  &&  event.repeatCount()
				&&  repeatDT.isValid()  &&  alarm.dateTime() > repeatDT)
				{
					// This deferral of a repeated alarm is later than the last occurrence
					// of the main alarm, so use the deferral alarm instead.
					// If the deferral is not yet due, this prevents the main alarm being
					// triggered repeatedly. If the deferral is due, this triggers it
					// in preference to the main alarm.
					alarmToExecute      = KAAlarm();
					alarmToExecuteValid = false;
					updateCalAndDisplay = false;
				}
				// Check if the alarm is due yet.
				int secs = alarm.dateTime().secsTo(now);
				if (secs < 0)
				{
					// The alarm appears to be in the future.
					// Check if it's an invalid local clock time during a daylight
					// saving time shift, which has actually passed.
					if (alarm.dateTime().timeSpec() != KDateTime::ClockTime
					||  alarm.dateTime() > now.toTimeSpec(KDateTime::ClockTime))
					{
						// This alarm is definitely not due yet
						kDebug(5950) << "KAlarmApp::handleEvent(): alarm " << alarm.type() << ": not due\n";
						continue;
					}
				}
				if (alarm.repeatAtLogin())
				{
					// Alarm is to be displayed at every login.
					// Check if the alarm has only just been set up.
					// (The alarm daemon will immediately notify that it is due
					//  since it is set up with a time in the past.)
					kDebug(5950) << "KAlarmApp::handleEvent(): REPEAT_AT_LOGIN\n";
					if (secs < maxLateness(1))
						continue;

					// Check if the main alarm is already being displayed.
					// (We don't want to display both at the same time.)
					if (alarmToExecute.valid())
						continue;

					// Set the time to be shown if it's a display alarm
					alarm.setTime(now);
				}
				if (event.repeatCount()  &&  alarm.type() == KAAlarm::MAIN_ALARM)
				{
					// Alarm has a simple repetition. Since its time in the calendr remains the same
					// until its repetitions are finished, adjust its time to the correct repetition
					KAEvent::OccurType type = event.previousOccurrence(now.addSecs(1), repeatDT, true);
					if (type & KAEvent::OCCURRENCE_REPEAT)
					{
						alarm.setTime(repeatDT);
						secs = repeatDT.secsTo(now);
					}
				}
				if (alarm.lateCancel())
				{
					// Alarm is due, and it is to be cancelled if too late.
					kDebug(5950) << "KAlarmApp::handleEvent(): LATE_CANCEL\n";
					bool late = false;
					bool cancel = false;
					if (alarm.dateTime().isDateOnly())
					{
						// The alarm has no time, so cancel it if its date is too far past
						int maxlate = alarm.lateCancel() / 1440;    // maximum lateness in days
						KDateTime limit(alarm.dateTime().addDays(maxlate + 1).effectiveKDateTime());
						if (now >= limit)
						{
							// It's too late to display the scheduled occurrence.
							// Find the last previous occurrence of the alarm.
							DateTime next;
							KAEvent::OccurType type = event.previousOccurrence(now, next, true);
							switch (type & ~KAEvent::OCCURRENCE_REPEAT)
							{
								case KAEvent::FIRST_OR_ONLY_OCCURRENCE:
								case KAEvent::RECURRENCE_DATE:
								case KAEvent::RECURRENCE_DATE_TIME:
								case KAEvent::LAST_RECURRENCE:
									limit.setDate(next.date().addDays(maxlate + 1));
									if (now >= limit)
									{
										if (type == KAEvent::LAST_RECURRENCE
										||  type == KAEvent::FIRST_OR_ONLY_OCCURRENCE && !event.recurs())
											cancel = true;   // last ocurrence (and there are no repetitions)
										else
											late = true;
									}
									break;
								case KAEvent::NO_OCCURRENCE:
								default:
									late = true;
									break;
							}
						}
					}
					else
					{
						// The alarm is timed. Allow it to be the permitted amount late before cancelling it.
						int maxlate = maxLateness(alarm.lateCancel());
						if (secs > maxlate)
						{
							// It's over the maximum interval late.
							// Find the most recent occurrence of the alarm.
							DateTime next;
							KAEvent::OccurType type = event.previousOccurrence(now, next, true);
							switch (type & ~KAEvent::OCCURRENCE_REPEAT)
							{
								case KAEvent::FIRST_OR_ONLY_OCCURRENCE:
								case KAEvent::RECURRENCE_DATE:
								case KAEvent::RECURRENCE_DATE_TIME:
								case KAEvent::LAST_RECURRENCE:
									if (next.effectiveKDateTime().secsTo(now) > maxlate)
									{
										if (type == KAEvent::LAST_RECURRENCE
										||  type == KAEvent::FIRST_OR_ONLY_OCCURRENCE && !event.recurs())
											cancel = true;   // last ocurrence (and there are no repetitions)
										else
											late = true;
									}
									break;
								case KAEvent::NO_OCCURRENCE:
								default:
									late = true;
									break;
							}
						}
					}

					if (cancel)
					{
						// All recurrences are finished, so cancel the event
						event.setArchive();
						cancelAlarm(event, alarm.type(), false);
						updateCalAndDisplay = true;
						continue;
					}
					if (late)
					{
						// The latest repetition was too long ago, so schedule the next one
						rescheduleAlarm(event, alarm, false);
						updateCalAndDisplay = true;
						continue;
					}
				}
				if (!alarmToExecuteValid)
				{
					kDebug(5950) << "KAlarmApp::handleEvent(): alarm " << alarm.type() << ": execute\n";
					alarmToExecute = alarm;             // note the alarm to be displayed
					alarmToExecuteValid = true;         // only trigger one alarm for the event
				}
				else
					kDebug(5950) << "KAlarmApp::handleEvent(): alarm " << alarm.type() << ": skip\n";
			}

			// If there is an alarm to execute, do this last after rescheduling/cancelling
			// any others. This ensures that the updated event is only saved once to the calendar.
			if (alarmToExecute.valid())
				execAlarm(event, alarmToExecute, true, !alarmToExecute.repeatAtLogin());
			else
			{
				if (function == EVENT_TRIGGER)
				{
					// The alarm is to be executed regardless of whether it's due.
					// Only trigger one alarm from the event - we don't want multiple
					// identical messages, for example.
					KAAlarm alarm = event.firstAlarm();
					if (alarm.valid())
						execAlarm(event, alarm, false);
				}
				if (updateCalAndDisplay)
					KAlarm::updateEvent(event, 0);     // update the window lists and calendar file
				else if (function != EVENT_TRIGGER)
				{
					kDebug(5950) << "KAlarmApp::handleEvent(): no action\n";
					Daemon::eventHandled(eventID);
				}
			}
			break;
		}
	}
	return true;
}

/******************************************************************************
* Called when an alarm action has completed, to perform any post-alarm actions.
*/
void KAlarmApp::alarmCompleted(const KAEvent& event)
{
	if (!event.postAction().isEmpty()  &&  ShellProcess::authorised())
	{
		QString command = event.postAction();
		kDebug(5950) << "KAlarmApp::alarmCompleted(" << event.id() << "): " << command << endl;
		doShellCommand(command, event, 0, ProcData::POST_ACTION);
	}
}

/******************************************************************************
* Reschedule the alarm for its next recurrence. If none remain, delete it.
* If the alarm is deleted and it is the last alarm for its event, the event is
* removed from the calendar file and from every main window instance.
*/
void KAlarmApp::rescheduleAlarm(KAEvent& event, const KAAlarm& alarm, bool updateCalAndDisplay)
{
	kDebug(5950) << "KAlarmApp::rescheduleAlarm()" << endl;
	bool update        = false;
	bool updateDisplay = false;
	if (alarm.reminder()  ||  alarm.deferred())
	{
		// It's an advance warning alarm or an extra deferred alarm, so delete it
		event.removeExpiredAlarm(alarm.type());
		update = true;
	}
	else if (alarm.repeatAtLogin())
	{
		// Leave an alarm which repeats at every login until its main alarm is deleted
		if (updateCalAndDisplay  &&  event.updated())
			update = true;
	}
	else
	{
		KDateTime now = KDateTime::currentUtcDateTime();
		if (event.repeatCount()  &&  event.mainEndRepeatTime() > now)
			updateDisplay = true;    // there are more repetitions to come, so just update time in alarm list
		else
		{
			// The alarm's repetitions (if any) are finished.
			// Reschedule it for its next recurrence.
			switch (event.setNextOccurrence(now))
			{
				case KAEvent::NO_OCCURRENCE:
					// All repetitions are finished, so cancel the event
					cancelAlarm(event, alarm.type(), updateCalAndDisplay);
					break;
				case KAEvent::RECURRENCE_DATE:
				case KAEvent::RECURRENCE_DATE_TIME:
				case KAEvent::LAST_RECURRENCE:
					// The event is due by now and repetitions still remain, so rewrite the event
					if (updateCalAndDisplay)
						update = true;
					else
					{
						event.cancelCancelledDeferral();
						event.setUpdated();    // note that the calendar file needs to be updated
					}
					break;
				case KAEvent::FIRST_OR_ONLY_OCCURRENCE:
					// The first occurrence is still due?!?, so don't do anything
				default:
					break;
			}
		}
		if (event.deferred())
		{
			// Just in case there's also a deferred alarm, ensure it's removed
			event.removeExpiredAlarm(KAAlarm::DEFERRED_ALARM);
			update = true;
		}
	}
	if (update)
	{
		event.cancelCancelledDeferral();
		KAlarm::updateEvent(event, 0);     // update the window lists and calendar file
	}
	else if (updateDisplay)
	{
		Daemon::eventHandled(event.id());
		AlarmListView::modifyEvent(event, 0);
	}
}

/******************************************************************************
* Delete the alarm. If it is the last alarm for its event, the event is removed
* from the calendar file and from every main window instance.
*/
void KAlarmApp::cancelAlarm(KAEvent& event, KAAlarm::Type alarmType, bool updateCalAndDisplay)
{
	kDebug(5950) << "KAlarmApp::cancelAlarm()" << endl;
	event.cancelCancelledDeferral();
	if (alarmType == KAAlarm::MAIN_ALARM  &&  !event.displaying()  &&  event.toBeArchived())
	{
		// The event is being deleted. Save it in the archived resources first.
		QString id = event.id();    // save event ID since KAlarm::addArchivedEvent() changes it
		KAlarm::addArchivedEvent(event);
		event.setEventID(id);       // restore event ID
	}
	event.removeExpiredAlarm(alarmType);
	if (!event.alarmCount())
		KAlarm::deleteEvent(event, false);
	else if (updateCalAndDisplay)
		KAlarm::updateEvent(event, 0);    // update the window lists and calendar file
}

/******************************************************************************
* Execute an alarm by displaying its message or file, or executing its command.
* Reply = ShellProcess instance if a command alarm
*       != 0 if successful
*       = 0 if the alarm is disabled, or if an error message was output.
*/
void* KAlarmApp::execAlarm(KAEvent& event, const KAAlarm& alarm, bool reschedule, bool allowDefer, bool noPreAction)
{
	if (!event.enabled())
	{
		// The event is disabled.
		if (reschedule)
			rescheduleAlarm(event, alarm, true);
		return 0;
	}

	void* result = (void*)1;
	event.setArchive();
	switch (alarm.action())
	{
		case KAAlarm::MESSAGE:
		case KAAlarm::FILE:
		{
			// Display a message or file, provided that the same event isn't already being displayed
			MessageWin* win = MessageWin::findEvent(event.id());
			if (!win  &&  !noPreAction  &&  !event.preAction().isEmpty()  &&  ShellProcess::authorised())
			{
				// There is no message window currently displayed for this alarm,
				// and we need to execute a command before displaying the new window.
				QString command = event.preAction();
				kDebug(5950) << "KAlarmApp::execAlarm(): pre-DISPLAY command: " << command << endl;
				int flags = (reschedule ? ProcData::RESCHEDULE : 0) | (allowDefer ? ProcData::ALLOW_DEFER : 0);
				if (doShellCommand(command, event, &alarm, (flags | ProcData::PRE_ACTION)))
					return result;     // display the message after the command completes
				// Error executing command - display the message even though it failed
			}
			if (!event.enabled())
				delete win;        // event is disabled - close its window
			else if (!win
			     ||  !win->hasDefer() && !alarm.repeatAtLogin()
			     ||  (win->alarmType() & KAAlarm::REMINDER_ALARM) && !(alarm.type() & KAAlarm::REMINDER_ALARM))
			{
				// Either there isn't already a message for this event,
				// or there is a repeat-at-login message with no Defer
				// button, which needs to be replaced with a new message,
				// or the caption needs to be changed from "Reminder" to "Message".
				if (win)
					win->setRecreating();    // prevent post-alarm actions
				delete win;
				int flags = (reschedule ? 0 : MessageWin::NO_RESCHEDULE) | (allowDefer ? 0 : MessageWin::NO_DEFER);
				(new MessageWin(event, alarm, flags))->show();
			}
			else
			{
				// Raise the existing message window and replay any sound
				win->repeat(alarm);    // N.B. this reschedules the alarm
			}
			break;
		}
		case KAAlarm::COMMAND:
		{
			int flags = event.commandXterm() ? ProcData::EXEC_IN_XTERM : 0;
			QString command = event.cleanText();
			if (event.commandScript())
			{
				// Store the command script in a temporary file for execution
				kDebug(5950) << "KAlarmApp::execAlarm(): COMMAND: (script)" << endl;
				QString tmpfile = createTempScriptFile(command, false, event, alarm);
				if (tmpfile.isEmpty())
				{
					QStringList errmsgs(i18n("Error creating temporary script file"));
					(new MessageWin(event, alarm.dateTime(), errmsgs))->show();
					result = 0;
				}
				else
					result = doShellCommand(tmpfile, event, &alarm, (flags | ProcData::TEMP_FILE));
			}
			else
			{
				kDebug(5950) << "KAlarmApp::execAlarm(): COMMAND: " << command << endl;
				result = doShellCommand(command, event, &alarm, flags);
			}
			if (reschedule)
				rescheduleAlarm(event, alarm, true);
			break;
		}
		case KAAlarm::EMAIL:
		{
			kDebug(5950) << "KAlarmApp::execAlarm(): EMAIL to: " << event.emailAddresses(", ") << endl;
			QStringList errmsgs;
			if (!KAMail::send(event, errmsgs, (reschedule || allowDefer)))
				result = 0;
			if (!errmsgs.isEmpty())
			{
				// Some error occurred, although the email may have been sent successfully
				if (result)
					kDebug(5950) << "KAlarmApp::execAlarm(): copy error: " << errmsgs[1] << endl;
				else
					kDebug(5950) << "KAlarmApp::execAlarm(): failed: " << errmsgs[1] << endl;
				(new MessageWin(event, alarm.dateTime(), errmsgs))->show();
			}
			if (reschedule)
				rescheduleAlarm(event, alarm, true);
			break;
		}
		default:
			return 0;
	}
	return result;
}

/******************************************************************************
* Execute a shell command line specified by an alarm.
* If the PRE_ACTION bit of 'flags' is set, the alarm will be executed via
* execAlarm() once the command completes, the execAlarm() parameters being
* derived from the remaining bits in 'flags'.
*/
ShellProcess* KAlarmApp::doShellCommand(const QString& command, const KAEvent& event, const KAAlarm* alarm, int flags)
{
	KProcess::Communication comms = KProcess::NoCommunication;
	QString cmd;
	QString tmpXtermFile;
	if (flags & ProcData::EXEC_IN_XTERM)
	{
		// Execute the command in a terminal window.
		cmd = Preferences::cmdXTermCommand();
		cmd.replace("%t", aboutData()->programName());     // set the terminal window title
		if (cmd.indexOf("%C") >= 0)
		{
			// Execute the command from a temporary script file
			if (flags & ProcData::TEMP_FILE)
				cmd.replace("%C", command);    // the command is already calling a temporary file
			else
			{
				tmpXtermFile = createTempScriptFile(command, true, event, *alarm);
				if (tmpXtermFile.isEmpty())
					return 0;
				cmd.replace("%C", tmpXtermFile);    // %C indicates where to insert the command
			}
		}
		else if (cmd.indexOf("%W") >= 0)
		{
			// Execute the command from a temporary script file,
			// with a sleep after the command is executed
			tmpXtermFile = createTempScriptFile(command + QLatin1String("\nsleep 86400\n"), true, event, *alarm);
			if (tmpXtermFile.isEmpty())
				return 0;
			cmd.replace("%W", tmpXtermFile);    // %w indicates where to insert the command
		}
		else if (cmd.indexOf("%w") >= 0)
		{
			// Append a sleep to the command.
			// Quote the command in case it contains characters such as [>|;].
			QString exec = KShellProcess::quote(command + QLatin1String("; sleep 86400"));
			cmd.replace("%w", exec);    // %w indicates where to insert the command string
		}
		else
		{
			// Set the command to execute.
			// Put it in quotes in case it contains characters such as [>|;].
			QString exec = KShellProcess::quote(command);
			if (cmd.indexOf("%c") >= 0)
				cmd.replace("%c", exec);    // %c indicates where to insert the command string
			else
				cmd.append(exec);           // otherwise, simply append the command string
		}
	}
	else
	{
		cmd = command;
		comms = KProcess::AllOutput;
	}
	ShellProcess* proc = new ShellProcess(cmd);
	connect(proc, SIGNAL(shellExited(ShellProcess*)), SLOT(slotCommandExited(ShellProcess*)));
	QPointer<ShellProcess> logproc = 0;
	if (comms == KProcess::AllOutput  &&  !event.logFile().isEmpty())
	{
		// Output is to be appended to a log file.
		// Set up a logging process to write the command's output to.
		connect(proc, SIGNAL(receivedStdout(KProcess*,char*,int)), SLOT(slotCommandOutput(KProcess*,char*,int)));
		connect(proc, SIGNAL(receivedStderr(KProcess*,char*,int)), SLOT(slotCommandOutput(KProcess*,char*,int)));
		logproc = new ShellProcess(QString::fromLatin1("cat >>%1").arg(event.logFile()));
		connect(logproc, SIGNAL(shellExited(ShellProcess*)), SLOT(slotLogProcExited(ShellProcess*)));
		logproc->start(KProcess::Stdin);
		QString heading;
		if (alarm  &&  alarm->dateTime().isValid())
		{
			QString dateTime = alarm->dateTime().formatLocale();
			heading.sprintf("\n******* KAlarm %s *******\n", dateTime.toLatin1().data());
		}
		else
			heading = QLatin1String("\n******* KAlarm *******\n");
		QByteArray hdg = heading.toLatin1();
		logproc->writeStdin(hdg, hdg.length());
	}
	ProcData* pd = new ProcData(proc, logproc, new KAEvent(event), (alarm ? new KAAlarm(*alarm) : 0), flags);
	if (flags & ProcData::TEMP_FILE)
		pd->tempFiles += command;
	if (!tmpXtermFile.isEmpty())
		pd->tempFiles += tmpXtermFile;
	mCommandProcesses.append(pd);
	if (proc->start(comms))
		return proc;

	// Error executing command - report it
	kError(5950) << "KAlarmApp::doShellCommand(): command failed to start\n";
	commandErrorMsg(proc, event, alarm, flags);
	mCommandProcesses.removeAt(mCommandProcesses.indexOf(pd));
	delete pd;
	return 0;
}

/******************************************************************************
* Create a temporary script file containing the specified command string.
* Reply = path of temporary file, or null string if error.
*/
QString KAlarmApp::createTempScriptFile(const QString& command, bool insertShell, const KAEvent& event, const KAAlarm& alarm)
{
	KTemporaryFile tmpFile;
	tmpFile.setAutoRemove(false);     // don't delete file when it is destructed
	if (!tmpFile.open())
		kError(5950) << "KAlarmApp::createTempScript(): Unable to create a temporary script file" << endl;
	else
	{
		tmpFile.setPermissions(QFile::ReadUser | QFile::WriteUser | QFile::ExeUser);
		QTextStream stream(&tmpFile);
		if (insertShell)
			stream << "#!" << ShellProcess::shellPath() << "\n";
		stream << command;
		stream.flush();
		if (tmpFile.error() != QFile::NoError)
			kError(5950) << "KAlarmApp::createTempScript(): Error " << tmpFile.errorString() << " writing to temporary script file" << endl;
		else
			return tmpFile.fileName();
	}

	QStringList errmsgs(i18n("Error creating temporary script file"));
	(new MessageWin(event, alarm.dateTime(), errmsgs))->show();
	return QString();
}

/******************************************************************************
* Called when an executing command alarm sends output to stdout or stderr.
*/
void KAlarmApp::slotCommandOutput(KProcess* proc, char* buffer, int bufflen)
{
//kDebug(5950) << "KAlarmApp::slotCommandOutput(): '" << QByteArray(buffer, bufflen) << "'\n";
	// Find this command in the command list
	for (int i = 0, end = mCommandProcesses.count();  i < end;  ++i)
	{
		ProcData* pd = mCommandProcesses[i];
		if (pd->process == proc  &&  pd->logProcess)
		{
			pd->logProcess->writeStdin(buffer, bufflen);
			break;
		}
	}
}

/******************************************************************************
* Called when a logging process completes.
*/
void KAlarmApp::slotLogProcExited(ShellProcess* proc)
{
	// Because it's held as a guarded pointer in the ProcData structure,
	// we don't need to set any pointers to zero.
	delete proc;
}

/******************************************************************************
* Called when a command alarm's execution completes.
*/
void KAlarmApp::slotCommandExited(ShellProcess* proc)
{
	kDebug(5950) << "KAlarmApp::slotCommandExited()\n";
	// Find this command in the command list
	for (int i = 0, end = mCommandProcesses.count();  i < end;  ++i)
	{
		ProcData* pd = mCommandProcesses[i];
		if (pd->process == proc)
		{
			// Found the command
			if (pd->logProcess)
				pd->logProcess->stdinExit();   // terminate the logging process

			// Check its exit status
			if (!proc->normalExit())
			{
				QString errmsg = proc->errorMessage();
				kWarning(5950) << "KAlarmApp::slotCommandExited(" << pd->event->cleanText() << "): " << errmsg << endl;
				if (pd->messageBoxParent)
				{
					// Close the existing informational KMessageBox for this process
					QList<KDialog*> dialogs = pd->messageBoxParent->findChildren<KDialog*>();
					if (!dialogs.isEmpty())
					    delete dialogs[0];
					if (!pd->tempFile())
					{
						errmsg += '\n';
						errmsg += proc->command();
					}
					KMessageBox::error(pd->messageBoxParent, errmsg);
				}
				else
					commandErrorMsg(proc, *pd->event, pd->alarm, pd->flags);
			}
			if (pd->preAction())
				execAlarm(*pd->event, *pd->alarm, pd->reschedule(), pd->allowDefer(), true);
			mCommandProcesses.removeAt(i);
			delete pd;
			break;
		}
	}

	// If there are now no executing shell commands, quit if a quit was queued
	if (mPendingQuit  &&  mCommandProcesses.isEmpty())
		quitIf(mPendingQuitCode);
}

/******************************************************************************
* Output an error message for a shell command.
*/
void KAlarmApp::commandErrorMsg(const ShellProcess* proc, const KAEvent& event, const KAAlarm* alarm, int flags)
{
	QStringList errmsgs;
	if (flags & ProcData::PRE_ACTION)
		errmsgs += i18n("Pre-alarm action:");
	else if (flags & ProcData::POST_ACTION)
		errmsgs += i18n("Post-alarm action:");
	errmsgs += proc->errorMessage();
	if (!(flags & ProcData::TEMP_FILE))
		errmsgs += proc->command();
	(new MessageWin(event, (alarm ? alarm->dateTime() : DateTime()), errmsgs))->show();
}

/******************************************************************************
* Notes that an informational KMessageBox is displayed for this process.
*/
void KAlarmApp::commandMessage(ShellProcess* proc, QWidget* parent)
{
	// Find this command in the command list
	for (int i = 0, end = mCommandProcesses.count();  i < end;  ++i)
	{
		ProcData* pd = mCommandProcesses[i];
		if (pd->process == proc)
		{
			pd->messageBoxParent = parent;
			break;
		}
	}
}

/******************************************************************************
* Set up remaining DCOP handlers and start processing DCOP calls.
*/
void KAlarmApp::setUpDcop()
{
	if (!mInitialised)
	{
		mInitialised = true;      // we're now ready to handle DCOP calls
		Daemon::createDcopHandler();
		QTimer::singleShot(0, this, SLOT(processQueue()));    // process anything already queued
	}
}

/******************************************************************************
* If this is the first time through, open the calendar file, optionally start
* the alarm daemon and register with it, and set up the DCOP handler.
*/
bool KAlarmApp::initCheck(bool calendarOnly)
{
	static bool firstTime = true;
	bool startdaemon;
	if (firstTime)
	{
		if (!mStartOfDay.isValid())
			changeStartOfDay();     // start of day time has changed, so adjust date-only alarms

		/* Need to open the display calendar now, since otherwise if the daemon
		 * immediately notifies display alarms, they will often be processed while
		 * MessageWin::redisplayAlarms() is executing open() (but before open()
		 * completes), which causes problems!!
		 */
		AlarmCalendar::displayCalendar()->open();

		AlarmCalendar::resources()->setPurgeDays(theInstance->mPrefsArchivedKeepDays);
		AlarmCalendar::resources()->open();

		startdaemon = true;
		firstTime = false;
	}
	else
		startdaemon = !Daemon::isRegistered();

	if (!calendarOnly)
	{
		setUpDcop();      // start processing DCOP calls
		if (startdaemon)
			Daemon::start();  // make sure the alarm daemon is running
	}
	return true;
}

/******************************************************************************
*  Convert the --time parameter string into a local date/time or date value.
*  The parameter is in the form [[[yyyy-]mm-]dd-]hh:mm or yyyy-mm-dd.
*  Reply = true if successful.
*/
static bool convWakeTime(const QByteArray& timeParam, KDateTime& dateTime, const KDateTime& defaultDt)
{
	int i = timeParam.indexOf(' ');
	if (i > 19)
		return false;
	QString zone = QString::fromLatin1(timeParam.mid(i));
	char timeStr[20];
	strcpy(timeStr, timeParam.left(i));
	int dt[5] = { -1, -1, -1, -1, -1 };
	char* s;
	char* end;
	bool noTime;
	// Get the minute value
	if ((s = strchr(timeStr, ':')) == 0)
		noTime = true;
	else
	{
		noTime = false;
		*s++ = 0;
		dt[4] = strtoul(s, &end, 10);
		if (end == s  ||  *end  ||  dt[4] >= 60)
			return false;
		// Get the hour value
		if ((s = strrchr(timeStr, '-')) == 0)
			s = timeStr;
		else
			*s++ = 0;
		dt[3] = strtoul(s, &end, 10);
		if (end == s  ||  *end  ||  dt[3] >= 24)
			return false;
	}
	bool noDate = true;
	if (s != timeStr)
	{
		noDate = false;
		// Get the day value
		if ((s = strrchr(timeStr, '-')) == 0)
			s = timeStr;
		else
			*s++ = 0;
		dt[2] = strtoul(s, &end, 10);
		if (end == s  ||  *end  ||  dt[2] == 0  ||  dt[2] > 31)
			return false;
		if (s != timeStr)
		{
			// Get the month value
			if ((s = strrchr(timeStr, '-')) == 0)
				s = timeStr;
			else
				*s++ = 0;
			dt[1] = strtoul(s, &end, 10);
			if (end == s  ||  *end  ||  dt[1] == 0  ||  dt[1] > 12)
				return false;
			if (s != timeStr)
			{
				// Get the year value
				dt[0] = strtoul(timeStr, &end, 10);
				if (end == timeStr  ||  *end)
					return false;
			}
		}
	}

	QDate date(dt[0], dt[1], dt[2]);
	QTime time(0, 0, 0);
	if (noTime)
	{
		// No time was specified, so the full date must have been specified
		if (dt[0] < 0  ||  !date.isValid())
			return false;
		dateTime = KAlarm::applyTimeZone(zone, date, time, false, defaultDt);
	}
	else
	{
		// Compile the values into a date/time structure
		time.setHMS(dt[3], dt[4], 0);
		if (dt[0] < 0)
		{
			// Some or all of the date was omitted.
			// Use the default date/time if provided.
			if (defaultDt.isValid())
			{
				dt[0] = defaultDt.date().year();
				date.setYMD(dt[0],
				            (dt[1] < 0 ? defaultDt.date().month() : dt[1]),
				            (dt[2] < 0 ? defaultDt.date().day() : dt[2]));
			}
			else
				date.setYMD(2000, 1, 1);  // temporary substitute for date
		}
		dateTime = KAlarm::applyTimeZone(zone, date, time, true, defaultDt);
		if (!dateTime.isValid())
			return false;
		if (dt[0] < 0)
		{
			// Some or all of the date was omitted.
			// Use the current date in the specified time zone as default.
			KDateTime now = KDateTime::currentDateTime(dateTime.timeSpec());
			date = dateTime.date();
			date.setYMD(now.date().year(),
			            (dt[1] < 0 ? now.date().month() : dt[1]),
			            (dt[2] < 0 ? now.date().day() : dt[2]));
			if (!date.isValid())
				return false;
			if (noDate  &&  time < now.time())
				date = date.addDays(1);
			dateTime.setDate(date);
		}
	}
	return dateTime.isValid();
}

/******************************************************************************
*  Convert a time interval command line parameter.
*  Reply = true if successful.
*/
static bool convInterval(QByteArray timeParam, KARecurrence::Type& recurType, int& timeInterval, bool allowMonthYear)
{
	// Get the recurrence interval
	bool ok = true;
	uint interval = 0;
	bool negative = (timeParam[0] == '-');
	if (negative)
		timeParam = timeParam.right(1);
	uint length = timeParam.length();
	switch (timeParam[length - 1])
	{
		case 'Y':
			if (!allowMonthYear)
				ok = false;
			recurType = KARecurrence::ANNUAL_DATE;
			timeParam = timeParam.left(length - 1);
			break;
		case 'W':
			recurType = KARecurrence::WEEKLY;
			timeParam = timeParam.left(length - 1);
			break;
		case 'D':
			recurType = KARecurrence::DAILY;
			timeParam = timeParam.left(length - 1);
			break;
		case 'M':
		{
			int i = timeParam.indexOf('H');
			if (i < 0)
			{
				if (!allowMonthYear)
					ok = false;
				recurType = KARecurrence::MONTHLY_DAY;
				timeParam = timeParam.left(length - 1);
			}
			else
			{
				recurType = KARecurrence::MINUTELY;
				interval = timeParam.left(i).toUInt(&ok) * 60;
				timeParam = timeParam.mid(i + 1, length - i - 2);
			}
			break;
		}
		default:       // should be a digit
			recurType = KARecurrence::MINUTELY;
			break;
	}
	if (ok)
		interval += timeParam.toUInt(&ok);
	timeInterval = static_cast<int>(interval);
	if (negative)
		timeInterval = -timeInterval;
	return ok;
}


KAlarmApp::ProcData::ProcData(ShellProcess* p, ShellProcess* logp, KAEvent* e, KAAlarm* a, int f)
	: process(p),
	  logProcess(logp),
	  event(e),
	  alarm(a),
	  messageBoxParent(0),
	  flags(f)
{ }

KAlarmApp::ProcData::~ProcData()
{
	while (!tempFiles.isEmpty())
	{
		// Delete the temporary file called by the XTerm command
		QFile f(tempFiles.first());
		f.remove();
		tempFiles.removeFirst();
	}
	delete process;
	delete event;
	delete alarm;
}

/*
 *  prefdlg.h  -  program preferences dialog
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

#ifndef PREFDLG_H
#define PREFDLG_H

#include <kpagedialog.h>

#include "preferences.h"
#include "recurrenceedit.h"
#include "soundpicker.h"

class QCheckBox;
class QAbstractButton;
class QRadioButton;
class QPushButton;
class QComboBox;
class QLineEdit;
class KTimeZone;
class KVBox;
class KColorCombo;
class FontColourChooser;
class ButtonGroup;
class TimeEdit;
class SpinBox;
class SpecialActionsButton;

class FontColourPrefTab;
class EditPrefTab;
class EmailPrefTab;
class ViewPrefTab;
class StorePrefTab;
class MiscPrefTab;


// The Preferences dialog
class KAlarmPrefDlg : public KPageDialog
{
		Q_OBJECT
	public:
		KAlarmPrefDlg();
		~KAlarmPrefDlg();

		FontColourPrefTab* mFontColourPage;
		EditPrefTab*       mEditPage;
		EmailPrefTab*      mEmailPage;
		ViewPrefTab*       mViewPage;
		StorePrefTab*      mStorePage;
		MiscPrefTab*       mMiscPage;

		KPageWidgetItem *  mFontColourPageItem;
		KPageWidgetItem *  mEditPageItem;
		KPageWidgetItem *  mEmailPageItem;
		KPageWidgetItem *  mViewPageItem;
		KPageWidgetItem *  mStorePageItem;
		KPageWidgetItem *  mMiscPageItem;

	protected slots:
		virtual void slotOk();
		virtual void slotApply();
		virtual void slotHelp();
		virtual void slotDefault();
		virtual void slotCancel();

	private:
		void            restore();
		bool            mValid;
};

// Base class for each tab in the Preferences dialog
class PrefsTabBase : public QWidget
{
		Q_OBJECT
	public:
		PrefsTabBase(KVBox*);

		void         setPreferences();
		virtual void restore() = 0;
		virtual void apply(bool syncToDisc) = 0;
		virtual void setDefaults() = 0;
		static int   indentWidth()    { return mIndentWidth; }

	protected:
		KVBox*       mPage;

	private:
		static int   mIndentWidth;       // indent width for checkboxes etc.
};


// Miscellaneous tab of the Preferences dialog
class MiscPrefTab : public PrefsTabBase
{
		Q_OBJECT
	public:
		MiscPrefTab(KVBox*);

		virtual void restore();
		virtual void apply(bool syncToDisc);
		virtual void setDefaults();

	private slots:
		void         slotAutostartDaemonClicked();
		void         slotRunModeToggled(bool);
		void         slotDisableIfStoppedToggled(bool);
		void         slotOtherTerminalToggled(bool);
//#ifdef AUTOSTART_BY_KALARMD
		void         slotAutostartToggled(bool);
//#endif

	private:
		void         setTimeZone(const KTimeZone*);

		QCheckBox*     mAutostartDaemon;
		QRadioButton*  mRunInSystemTray;
		QRadioButton*  mRunOnDemand;
		QCheckBox*     mDisableAlarmsIfStopped;
		QCheckBox*     mQuitWarn;
		QCheckBox*     mAutostartTrayIcon;
		QCheckBox*     mConfirmAlarmDeletion;
		QComboBox*     mTimeZone;
		TimeEdit*      mStartOfDay;
		ButtonGroup*   mXtermType;
		QLineEdit*     mXtermCommand;
		int            mXtermCount;              // number of terminal window types
};


// Storage tab of the Preferences dialog
class StorePrefTab : public PrefsTabBase
{
		Q_OBJECT
	public:
		StorePrefTab(KVBox*);

		virtual void restore();
		virtual void apply(bool syncToDisc);
		virtual void setDefaults();

	private slots:
		void         slotArchivedToggled(bool);
		void         slotClearArchived();

	private:
		void         setArchivedControls(int purgeDays);

		QRadioButton*  mDefaultResource;
		QRadioButton*  mAskResource;
		QCheckBox*     mKeepArchived;
		QCheckBox*     mPurgeArchived;
		SpinBox*       mPurgeAfter;
		QLabel*        mPurgeAfterLabel;
		QPushButton*   mClearArchived;
		bool           mOldKeepArchived;    // previous setting of keep-archived
		bool           mCheckKeepChanges;
};


// Email tab of the Preferences dialog
class EmailPrefTab : public PrefsTabBase
{
		Q_OBJECT
	public:
		EmailPrefTab(KVBox*);

		QString      validate();
		virtual void restore();
		virtual void apply(bool syncToDisc);
		virtual void setDefaults();

	private slots:
		void         slotEmailClientChanged(QAbstractButton*);
		void         slotFromAddrChanged(QAbstractButton*);
		void         slotBccAddrChanged(QAbstractButton*);
		void         slotAddressChanged()    { mAddressChanged = true; }

	private:
		void         setEmailAddress(Preferences::MailFrom, const QString& address);
		void         setEmailBccAddress(bool useControlCentre, const QString& address);
		QString      validateAddr(ButtonGroup*, QLineEdit* addr, const QString& msg);

		ButtonGroup*   mEmailClient;
		RadioButton*   mKMailButton;
		RadioButton*   mSendmailButton;
		ButtonGroup*   mFromAddressGroup;
		RadioButton*   mFromAddrButton;
		RadioButton*   mFromCCentreButton;
		RadioButton*   mFromKMailButton;
		QLineEdit*     mEmailAddress;
		ButtonGroup*   mBccAddressGroup;
		RadioButton*   mBccAddrButton;
		RadioButton*   mBccCCentreButton;
		QLineEdit*     mEmailBccAddress;
		QCheckBox*     mEmailQueuedNotify;
		QCheckBox*     mEmailCopyToKMail;
		bool           mAddressChanged;
		bool           mBccAddressChanged;
};


// Edit defaults tab of the Preferences dialog
class EditPrefTab : public PrefsTabBase
{
		Q_OBJECT
	public:
		EditPrefTab(KVBox*);

		QString      validate();
		virtual void restore();
		virtual void apply(bool syncToDisc);
		virtual void setDefaults();

	private slots:
		void         slotBrowseSoundFile();

	private:
		QCheckBox*      mAutoClose;
		QCheckBox*      mConfirmAck;
		QComboBox*      mReminderUnits;
		SpecialActionsButton* mSpecialActionsButton;
		QCheckBox*      mCmdScript;
		QCheckBox*      mCmdXterm;
		QCheckBox*      mEmailBcc;
		QComboBox*      mSound;
		QLabel*         mSoundFileLabel;
		QLineEdit*      mSoundFile;
		QPushButton*    mSoundFileBrowse;
		QCheckBox*      mSoundRepeat;
		QCheckBox*      mCopyToKOrganizer;
		QCheckBox*      mLateCancel;
		QComboBox*      mRecurPeriod;
		ButtonGroup*    mFeb29;

		static int soundIndex(SoundPicker::Type);
		static int recurIndex(RecurrenceEdit::RepeatType);
};


// View tab of the Preferences dialog
class ViewPrefTab : public PrefsTabBase
{
		Q_OBJECT
	public:
		ViewPrefTab(KVBox*);

		virtual void restore();
		virtual void apply(bool syncToDisc);
		virtual void setDefaults();

	private slots:
		void         slotListTimeToggled(bool);
		void         slotListTimeToToggled(bool);
		void         slotTooltipAlarmsToggled(bool);
		void         slotTooltipMaxToggled(bool);
		void         slotTooltipTimeToggled(bool);
		void         slotTooltipTimeToToggled(bool);

	private:
		void         setList(bool time, bool timeTo);
		void         setTooltip(int maxAlarms, bool time, bool timeTo, const QString& prefix);

		QCheckBox*     mShowResources;
		QCheckBox*     mListShowTime;
		QCheckBox*     mListShowTimeTo;
		QCheckBox*     mTooltipShowAlarms;
		QCheckBox*     mTooltipMaxAlarms;
		SpinBox*       mTooltipMaxAlarmCount;
		QCheckBox*     mTooltipShowTime;
		QCheckBox*     mTooltipShowTimeTo;
		QLineEdit*     mTooltipTimeToPrefix;
		QLabel*        mTooltipTimeToPrefixLabel;
		QCheckBox*     mModalMessages;
		QCheckBox*     mShowArchivedAlarms;
		SpinBox*       mDaemonTrayCheckInterval;
};


// Font & Colour tab of the Preferences dialog
class FontColourPrefTab : public PrefsTabBase
{
		Q_OBJECT
	public:
		FontColourPrefTab(KVBox*);

		virtual void restore();
		virtual void apply(bool syncToDisc);
		virtual void setDefaults();

	private:
		FontColourChooser*  mFontChooser;
		KColorCombo*        mDisabledColour;
		KColorCombo*        mArchivedColour;
};

#endif // PREFDLG_H

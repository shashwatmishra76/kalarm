/*
 *  functions_p.h  -  private declarations for miscellaneous functions
 *  Program:  kalarm
 *  Copyright © 2009 by David Jarvie <djarvie@kde.org>
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

#ifndef FUNCTIONS_P_H
#define FUNCTIONS_P_H

#include "kalarm.h"   //krazy:exclude=includes (kalarm.h must be first)
#ifdef Q_WS_X11
#include <kwindowsystem.h>
#endif
#include <QObject>

namespace KAlarm
{

// Private class which exists solely to allow signals/slots to work.
class Private : public QObject
{
	Q_OBJECT
    public:
	Private(QObject* parent = 0) : QObject(parent) {}
	static bool startKMailMinimised();
	static Private* instance()
	{
		if (!mInstance)
			mInstance = new Private;
		return mInstance;
	}

    public slots:
#ifdef Q_WS_X11
	void windowAdded(WId);
#endif

    private:
	static Private* mInstance;
};

} // namespace KAlarm

#endif // FUNCTIONS_P_H

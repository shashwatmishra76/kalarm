/*
 *  eventlistview.cpp  -  base class for widget showing list of alarms
 *  Program:  kalarm
 *  Copyright © 2007 by David Jarvie <software@astrojar.org.uk>
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

#include <QHeaderView>
#include <QMouseEvent>
#include <QApplication>

#include <kdebug.h>

#include "eventlistmodel.h"
#include "templatelistfiltermodel.h"
#include "eventlistview.moc"


EventListView::EventListView(QWidget* parent)
	: QTreeView(parent)
{
	setRootIsDecorated(false);    // don't show expander icons for child-less items
	setSortingEnabled(true);
	setAllColumnsShowFocus(true);
	setSelectionMode(ExtendedSelection);
	setSelectionBehavior(SelectRows);
	setTextElideMode(Qt::ElideRight);
}

/******************************************************************************
* Return the event referred to by an index.
*/
KCal::Event* EventListView::event(const QModelIndex& index) const
{
	return eventFilterModel()->event(index);
}

KCal::Event* EventListView::event(int row) const
{
	return eventFilterModel()->event(row);
}

/******************************************************************************
* Select one event and make it the current item.
*/
void EventListView::select(const QString& eventId)
{
	select(eventModel()->eventIndex(eventId));
}

void EventListView::select(const QModelIndex& index)
{
	selectionModel()->select(index, QItemSelectionModel::SelectCurrent | QItemSelectionModel::Rows);
}

/******************************************************************************
* Return the single selected item.
* Reply = invalid if no items are selected, or if multiple items are selected.
*/
QModelIndex EventListView::selectedIndex() const
{
	QModelIndexList list = selectionModel()->selectedRows();
	if (list.count() != 1)
		return QModelIndex();
	return list[0];
}

/******************************************************************************
* Return the single selected event.
* Reply = null if no items are selected, or if multiple items are selected.
*/
KCal::Event* EventListView::selectedEvent() const
{
	QModelIndexList list = selectionModel()->selectedRows();
	if (list.count() != 1)
		return 0;
kDebug()<<"SelectedEvent() count="<<list.count()<<endl;
	const QAbstractProxyModel* proxy = static_cast<const QAbstractProxyModel*>(list[0].model());
	QModelIndex source = proxy->mapToSource(list[0]);
	return static_cast<KCal::Event*>(source.internalPointer());
}

/******************************************************************************
* Return the selected events.
*/
KCal::Event::List EventListView::selectedEvents() const
{
	KCal::Event::List elist;
	QModelIndexList ilist = selectionModel()->selectedRows();
	int count = ilist.count();
	if (count)
	{
		const QAbstractProxyModel* proxy = static_cast<const QAbstractProxyModel*>(ilist[0].model());
		for (int i = 0;  i < count;  ++i)
		{
			QModelIndex source = proxy->mapToSource(ilist[i]);
			elist += static_cast<KCal::Event*>(source.internalPointer());
		}
	}
	return elist;
}

/******************************************************************************
* Called when a mouse button is released.
*/
void EventListView::mouseReleaseEvent(QMouseEvent* e)
{
	if (e->button() == Qt::RightButton)
		emit rightButtonClicked(e->globalPos());
	else
		QTreeView::mouseReleaseEvent(e);
}
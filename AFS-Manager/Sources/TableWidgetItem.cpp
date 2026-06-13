#include <TableWidgetItem.h>
#include <Shared.h>

#include <QDateTime>

using namespace Shared;

TableWidgetItem::TableWidgetItem(const QString &text, TableWidgetItem::Type type) : QTableWidgetItem(text), type(type)
{
}

bool TableWidgetItem::operator<(const QTableWidgetItem &other) const
{
	if (type == Type::Integer) {
		return getSize(text().toLocal8Bit().toStdString()) < getSize(other.text().toLocal8Bit().toStdString());
	}
	else if (type == Type::DateTime) {
		return data(Qt::EditRole).toDateTime() < other.data(Qt::EditRole).toDateTime();
	}
	else {
		return QTableWidgetItem::operator<(other);
	}
}

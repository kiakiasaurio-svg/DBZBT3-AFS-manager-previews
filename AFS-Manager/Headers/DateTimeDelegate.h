#ifndef DATETIMEDELEGATE_H
#define DATETIMEDELEGATE_H

#include <QStyledItemDelegate>

/*
 * DateTimeDelegate
 *
 * Renders a QDateTime value (stored in Qt::EditRole/Qt::DisplayRole) as plain
 * text in the cell, and only instantiates a QDateTimeEdit editor widget when
 * the user actually starts editing the cell (double-click).
 *
 * This avoids creating one persistent QDateTimeEdit widget per row, which is
 * extremely expensive (memory and layout cost) for AFS files containing
 * thousands of entries.
 */
class DateTimeDelegate : public QStyledItemDelegate
{
Q_OBJECT

public:
	explicit DateTimeDelegate(QObject *parent = nullptr);

	QString displayText(const QVariant &value, const QLocale &locale) const override;

	QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

	void setEditorData(QWidget *editor, const QModelIndex &index) const override;

	void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override;
};

#endif // DATETIMEDELEGATE_H

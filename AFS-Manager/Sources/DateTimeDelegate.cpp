#include <DateTimeDelegate.h>

#include <QDateTimeEdit>
#include <QDateTime>

DateTimeDelegate::DateTimeDelegate(QObject *parent) : QStyledItemDelegate(parent)
{
}

QString DateTimeDelegate::displayText(const QVariant &value, const QLocale &locale) const
{
	if (value.canConvert<QDateTime>()) {
		return locale.toString(value.toDateTime(), "dd-MM-yyyy HH:mm:ss");
	}

	return QStyledItemDelegate::displayText(value, locale);
}

QWidget *DateTimeDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	Q_UNUSED(option);
	Q_UNUSED(index);

	auto *editor = new QDateTimeEdit(parent);
	editor->setFrame(false);
	editor->setCalendarPopup(true);

	return editor;
}

void DateTimeDelegate::setEditorData(QWidget *editor, const QModelIndex &index) const
{
	auto *dateTimeEdit = static_cast<QDateTimeEdit *>(editor);
	dateTimeEdit->setDateTime(index.data(Qt::EditRole).toDateTime());
}

void DateTimeDelegate::setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const
{
	auto *dateTimeEdit = static_cast<QDateTimeEdit *>(editor);

	model->setData(index, dateTimeEdit->dateTime(), Qt::EditRole);
	model->setData(index, dateTimeEdit->dateTime(), Qt::DisplayRole);
}

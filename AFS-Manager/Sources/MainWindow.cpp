#include <MainWindow.h>
#include <ui_MainWindow.h>

#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QCloseEvent>
#include <QMimeData>
#include <QPushButton>
#include <QLineEdit>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QStandardPaths>
#include <QDebug>

#include <algorithm>
#include <chrono>

#include <AboutDialog.h>
#include <MessageBox.h>
#include <ProgressDialog.h>
#include <ReservedSpaceDialog.h>

#include <ADXCore.h>
#include <ISOCore.h>

using namespace Shared;

enum columnID
{
	number, filename, size, reservedSpace, afterRebuild, dateModified, address
};

MainWindow::MainWindow(const std::string &name, const std::string &version, QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow), afs(nullptr), enableCellChanged(false), oldWorker(nullptr), adxPreview(new AdxPreview(this)), isoModified(false)
{
	// connect actions to context menu and setup ui
	ui->setupUi(this);

	// set title
	std::string title = name + " v" + version + " [WIP]";
#ifdef DBZBT3_DEBUG
	title += " DEBUG";
#endif
	this->setWindowTitle(title.c_str());

	// disable menu tools
	ui->menuTools->setEnabled(false);

	setAcceptDrops(true);

	// use a lazy editor delegate for the "Date modified" column instead of
	// creating a persistent QDateTimeEdit widget per row (very expensive for
	// AFS files with thousands of entries)
	ui->tableWidget->setItemDelegateForColumn(columnID::dateModified, new DateTimeDelegate(this));

	// preview wiring
	ui->previewButton->setEnabled(false);

	connect(ui->tableWidget, &QTableWidget::itemSelectionChanged, this, &MainWindow::updatePreviewAvailability);
	connect(ui->previewButton, &QPushButton::clicked, this, &MainWindow::on_actionPreview_triggered);

	connect(adxPreview, &AdxPreview::errorMessage, this, [this](const QString &message) {
		ShowError(this, "Error", message);
	});

	// search/filter wiring
	connect(ui->searchBox, &QLineEdit::textChanged, this, &MainWindow::applySearchFilter);
}

MainWindow::~MainWindow()
{
	delPointer(afs);
	delPointer(ui);
}

void MainWindow::openAFS(const std::string &path, bool firstCall)
{
	if (path.empty()) {
		return;
	}

	auto *afs = new AFS_File(path);
	auto error = afs->getError();

	qDebug() << afs->afsName.c_str();

	if (error.badStream || error.invalidAddress || error.notAFS || error.unableToOpen) {
		delPointer(afs);
		if (error.badStream) {
			ShowError(this, "Error", "Error while reading file");
		}
		else if (error.notAFS || error.invalidAddress) {
			ShowError(this, "Error", "This is not a valid AFS");
		}
		else if (error.unableToOpen) {
			ShowError(this, "Error", "Unable to open AFS");
		}
		return;
	}

	if (error.corruptedContent && ShowError(this, "Error", "This AFS seems to be corrupted...\nDo you want to load anyway?", QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
		delPointer(afs);
		return;
	}

	if (error.coherency || error.invalidDesc || error.overSize) {
		qDebug() << "error.coherency" << error.coherency << "| error.invalidDesc" << error.invalidDesc << "| error.overSize" << error.overSize;

		if (!firstCall || ShowWarning(this, "Error", "This AFS needs to be fixed\nDo you want to do it?", QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
			bool result = false;

			if (error.invalidDesc) {
				if ((result = afs->fixInvalidDesc())) {
					delPointer(afs);
					ShowInfo(this, "Success", "AFS descriptor regenerated successfully!");
					openAFS(path, false);
					return;
				}
			}
			else {
				if (error.coherency) {
					result = afs->commitFileInfo() && afs->commitFileDesc();
				}
				if (error.overSize && (error.coherency ? result : true)) {
					result = afs->fixOverSize();
				}
			}

			if (result && firstCall) {
				ShowInfo(this, "Success", "AFS fixed successfully!");
			}
			else if (!result) {
				delPointer(afs);
				ShowError(this, "Error", "Unable to fix AFS...");
				return;
			}
		}
		else {
			delPointer(afs);
			return;
		}
	}

	ui->menuTools->setEnabled(false);
	delPointer(this->afs);
	this->afs = afs;
	ui->menuTools->setEnabled(true);

	// opening a standalone AFS clears any ISO association
	if (!tempAFSPath.empty()) {
		QFile::remove(QString::fromLocal8Bit(tempAFSPath.c_str()));
		tempAFSPath.clear();
	}
	currentISOPath.clear();
	currentISOEntry = ISO_File::FileEntry{};
	isoModified = false;
	ui->actionSaveAFStoISO->setEnabled(false);

	adxPreview->stop();

	// clear any active search filter from a previously opened AFS;
	// blockSignals avoids triggering applySearchFilter before drawFileList runs
	ui->searchBox->blockSignals(true);
	ui->searchBox->clear();
	ui->searchBox->blockSignals(false);

	auto start = std::chrono::steady_clock::now();

	//setCursor(QCursor(Qt::WaitCursor));

	drawFileList();
	updateFreeSpaceLabel();

	//setCursor(QCursor(Qt::ArrowCursor));

	auto end = std::chrono::steady_clock::now();

	ui->loadingTime->setText("Loading time: " + QString::number((double)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000.0) + " sec");
}

bool MainWindow::rebuildAFS(AFS_File *afs)
{
	bool newAFS = this->afs != afs;

	if (afs == nullptr) {
		if (this->afs != nullptr) {
			afs = this->afs;
		}
		else {
			return false; // prevent possible future errors
		}
	}

	std::string path;
	do {
		path = QFileDialog::getSaveFileName(this, "Save rebuilded AFS file", getFilename(afs->afsName).c_str(), "AFS file (*.afs)").toLocal8Bit().toStdString();
		if (afs->afsName == path) {
			ShowError(this, "Error", "Unable to rebuild AFS over the original!\nSelect another location and try again");
		}
	} while (afs->afsName == path);

	if (path.empty()) {
		return false;
	}

	if (newAFS) {
		startWorker(Type::Rebuild, {{0, path}}, afs);
	}
	else {
		startWorker(Type::Rebuild, {{0, path}});
	}

	return true;
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
	event->acceptProposedAction();
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
	auto pos = event->pos();
	pos.ry() -= ui->tableWidget->horizontalHeader()->height();
	pos = ui->tableWidget->mapFrom(this, pos);

	int row = ui->tableWidget->rowAt(pos.y());
	int column = ui->tableWidget->columnAt(pos.x());

	ui->tableWidget->clearSelection();

	if (row != -1 && column != -1) {
		auto tot = row + event->mimeData()->urls().size() - 1;
		auto max = ui->tableWidget->rowCount() - 1;

		if (tot > max) {
			tot = max;
		}

		QTableWidgetSelectionRange range(row, 0, tot, ui->tableWidget->columnCount() - 1);
		ui->tableWidget->setRangeSelected(range, true);
	}

	event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
	auto urls = event->mimeData()->urls();
	auto size = (uint32_t)urls.size();

	auto pos = event->pos();
	pos.ry() -= ui->tableWidget->horizontalHeader()->height();
	pos = ui->tableWidget->mapFrom(this, pos);

	int row = ui->tableWidget->rowAt(pos.y());
	int column = ui->tableWidget->columnAt(pos.x());

	if (afs == nullptr || (size == 1 && (row == -1 || column == -1))) {
		std::string path = urls[0].path().toLocal8Bit().toStdString();

#ifdef _WIN32
		if (path[0] == '/') {
			path = path.substr(1, path.size());
		}
#endif

		openAFS(path);
	}
	else {
		if (row != -1 && column != -1) {
			std::map<uint32_t, std::string> list;
			std::string path;

			for (uint32_t i = 0; i < size; ++i) {
				try {
					path = urls[i].path().toLocal8Bit().toStdString();

#ifdef _WIN32
					if (path[0] == '/') {
						path = path.substr(1, path.size());
					}
#endif
					list.insert({getIndexFromRow(row + i), path});
				} catch (...) {
					// just skip invalid row
				}
			}

			startWorker(Type::Import, list);
		}
	}

	event->acceptProposedAction();
}

void MainWindow::populateRowCell(int row, int column, QWidget *widget)
{
	ui->tableWidget->setCellWidget(row, column, widget);
}

void MainWindow::populateRowCell(int row, int column, QTableWidgetItem *item)
{
	item->setFlags(item->flags() ^ Qt::ItemIsEditable);
	item->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	ui->tableWidget->setItem(row, column, item);
}

void MainWindow::adjustColumns()
{
	/*ui->tableWidget->horizontalHeader()->setResizeContentsPrecision(-1);
	ui->tableWidget->resizeColumnsToContents();*/

	ui->tableWidget->setColumnWidth(columnID::number, ui->tableWidget->columnWidth(columnID::number) + 30);
	ui->tableWidget->setColumnWidth(columnID::filename, ui->tableWidget->columnWidth(columnID::filename) + 100);
	ui->tableWidget->setColumnWidth(columnID::size, ui->tableWidget->columnWidth(columnID::size) + 35);
	ui->tableWidget->setColumnWidth(columnID::reservedSpace, ui->tableWidget->columnWidth(columnID::reservedSpace) + 35);
	ui->tableWidget->setColumnWidth(columnID::afterRebuild, ui->tableWidget->columnWidth(columnID::afterRebuild) + 35);
	ui->tableWidget->setColumnWidth(columnID::dateModified, ui->tableWidget->columnWidth(columnID::dateModified) + 40);
	ui->tableWidget->setColumnWidth(columnID::address, ui->tableWidget->columnWidth(columnID::address) + 35);
}

void MainWindow::drawFileList()
{
	if (afs == nullptr) {
		return; // prevent possible future errors
	}

	enableCellChanged = false;

	// block signals (itemSelectionChanged, cellChanged, etc.) during bulk repopulation:
	// without this, rebuilding thousands of rows/items can repeatedly trigger
	// itemSelectionChanged -> updatePreviewAvailability, causing severe slowdowns
	// on large AFS files
	ui->tableWidget->blockSignals(true);

	ui->tableWidget->setSortingEnabled(false);

	ui->tableWidget->clear();

	// get afs info
	auto vfi = afs->getFileInfo();
	auto vfd = afs->getFileDesc();
	auto fileCount = afs->getFileCount();

	// set text information
	ui->afsName->setText(afs->afsName.c_str());
	ui->afsFileCount->setText("File count: " + QString::number(fileCount));
	ui->afsSize->setText(("AFS size: " + getSize(afs->getAFSSize())).c_str());

#ifdef DBZBT3_DEBUG
	vfd.emplace_back();
	fileCount++;
#endif

	// generate columns
	auto columns = QString("N.;Filename;Size;Reserved space;Reserved space\n(after rebuild);Date modified;Address").split(";");
	ui->tableWidget->setColumnCount(columns.size());
	ui->tableWidget->setHorizontalHeaderLabels(columns);

	// set default row height and alignment
	ui->tableWidget->insertRow(0);
	//ui->tableWidget->resizeRowToContents(0);
	ui->tableWidget->verticalHeader()->setDefaultAlignment(Qt::AlignHCenter);
	ui->tableWidget->verticalHeader()->setDefaultSectionSize(ui->tableWidget->rowHeight(0));
	ui->tableWidget->resizeColumnsToContents();

	// generate rows
	ui->tableWidget->setRowCount(fileCount);


	for (uint32_t i = 0; i < fileCount; ++i) {
		// element
		QTableWidgetItem *item = new TableWidgetItem(QString::number(i + 1), TableWidgetItem::Type::Integer);
		populateRowCell(i, columnID::number, item);

		// filename
		item = new TableWidgetItem(QString::fromLocal8Bit(afs->getFilename(i).c_str()));
		populateRowCell(i, columnID::filename, item);
		item->setFlags(item->flags() ^ Qt::ItemIsEditable);
		item->setTextAlignment(item->textAlignment() ^ Qt::AlignHCenter);

		// size
		//item = new TableWidgetItem(QString::number(vfi[i].size), TableWidgetItem::Type::Integer);
		item = new TableWidgetItem(getSize(vfi[i].size).c_str(), TableWidgetItem::Type::Integer);
		populateRowCell(i, columnID::size, item);

		auto hasOverSpace = afs->hasOverSpace(i);

		// reservedSpace
		//item = new TableWidgetItem(QString::number(vfi[i].reservedSpace), TableWidgetItem::Type::Integer);
		item = new TableWidgetItem(getSize(vfi[i].reservedSpace).c_str(), TableWidgetItem::Type::Integer);
		populateRowCell(i, columnID::reservedSpace, item);
		if (!hasOverSpace.first) {
			item->setTextColor(Qt::GlobalColor::green);
		}

		// afterRebuild
		//item = new TableWidgetItem(QString::number(vfi[i].reservedSpaceRebuild), TableWidgetItem::Type::Integer);
		item = new TableWidgetItem(getSize(vfi[i].reservedSpaceRebuild).c_str(), TableWidgetItem::Type::Integer);
		populateRowCell(i, columnID::afterRebuild, item);
		if (!hasOverSpace.second) {
			item->setTextColor(Qt::GlobalColor::green);
		}

		// date
		QDate date(vfd[i].year, vfd[i].month, vfd[i].day);
		QTime time(vfd[i].hour, vfd[i].min, vfd[i].sec);
		item = new TableWidgetItem(QString(), TableWidgetItem::Type::DateTime);
		item->setData(Qt::EditRole, QDateTime(date, time));
		item->setData(Qt::DisplayRole, QDateTime(date, time));
		populateRowCell(i, columnID::dateModified, item);
		item->setFlags(item->flags() ^ Qt::ItemIsEditable);

		// fileAddress
		item = new TableWidgetItem(QString::number(vfi[i].address), TableWidgetItem::Type::Integer);
		populateRowCell(i, columnID::address, item);
	}

	// adjust columns
	adjustColumns();

	ui->tableWidget->setSortingEnabled(true);

	ui->tableWidget->blockSignals(false);

	enableCellChanged = true;

	// selection was reset while signals were blocked, so update the Preview
	// button state once now that signals are re-enabled
	updatePreviewAvailability();

	// re-apply the current search filter to the newly created rows
	applySearchFilter(ui->searchBox->text());
}

uint32_t MainWindow::getIndexFromRow(int row) const
{
	auto item = ui->tableWidget->item(row, columnID::number);

	if (item != nullptr) {
		return (item->text().toUInt() - 1);
	}

	throw std::exception();
}

std::vector<uint32_t> MainWindow::getSelectedIndexes() const
{
	QModelIndexList selection = ui->tableWidget->selectionModel()->selectedRows();
	uint32_t size = selection.size();

	std::vector<uint32_t> rows;
	rows.reserve(size);

	for (uint32_t i = 0; i < size; ++i) {
		rows.emplace_back(getIndexFromRow(selection[i].row()));
	}

	return rows;
}

void MainWindow::startWorker(Type type, const std::map<uint32_t, std::string> &list, AFS_File *afs, const QString &cleanupPath)
{
	if (afs == nullptr) {
		if (this->afs != nullptr) {
			afs = this->afs;
		}
		else {
			return; // prevent possible future errors
		}
	}

	if (list.empty() || (type == Type::Rebuild && list.size() != 1)) {
		return;
	}

	auto *worker = new Worker(type, afs, list, this);
	auto *progressDialog = new ProgressDialog(type, list.size(), 0, this);

	connect(worker, &Worker::started, progressDialog, &ProgressDialog::show); // show dialog when thread start
	connect(worker, &Worker::done, progressDialog, &ProgressDialog::accept); // close dialog when thread finish

	connect(this, &MainWindow::skipFile, worker, &Worker::skipFile); // send skip signal to worker
	connect(worker, &Worker::errorFile, this, &MainWindow::errorFile); // show warning on error

	if (type != Type::Rebuild) {
		connect(worker, &Worker::next, progressDialog, &ProgressDialog::next); // updated bar on progress
		connect(worker, &Worker::errorMessage, this, &MainWindow::errorMessage); // catch exceptions

		if (type == Type::Import) {
			connect(worker, &Worker::toAdjust, this, &MainWindow::toAdjust_p1); // ask to rebuild
			connect(worker, &Worker::refreshRow, this, &MainWindow::refreshRow); // refresh row of imported file
			connect(worker, &Worker::refreshRow, this, &MainWindow::updateFreeSpaceLabel); // update free space label on imported file
		}
	}
	else {
		if (oldWorker != nullptr) {
			connect(worker, &Worker::rebuilded, this, &MainWindow::toAdjust_p2); // complete import
		}
	}

	connect(worker, &Worker::progressText, progressDialog, &ProgressDialog::setLabel); // update label on progress

	connect(progressDialog, &ProgressDialog::rejected, worker, &Worker::terminate); // kill thread on dialog rejected
	connect(worker, &Worker::abort, this, &MainWindow::abort); // are you sure that you want to abort?

	connect(this, &MainWindow::done, worker, &Worker::deleteLater); // clean worker on abort accepted

	connect(worker, &Worker::done, worker, &Worker::deleteLater); // clean worker on finish
	connect(worker, &Worker::destroyed, progressDialog, &ProgressDialog::deleteLater); // clean dialog after thread cleaned

	if (!cleanupPath.isEmpty()) {
		// remove a temporary file (e.g. a converted ADX) once the worker finishes
		connect(worker, &Worker::done, this, [cleanupPath]() {
			QFile::remove(cleanupPath);
		});
	}

	worker->start();
}

// ---------- menu bar ----------
void MainWindow::on_actionOpen_triggered()
{
	openAFS(QFileDialog::getOpenFileName(this).toLocal8Bit().toStdString());
}

void MainWindow::on_actionExit_triggered()
{
	this->close();
}

void MainWindow::on_actionSettings_triggered()
{
	ShowError(this, "Error", "Not yet implemented...");
}

void MainWindow::on_actionExportToFolder_triggered()
{
	if (afs == nullptr) {
		return; // prevent possible future errors
	}

	std::string path = QFileDialog::getExistingDirectory(this).toLocal8Bit().toStdString();

	if (path.empty()) {
		return;
	}

	auto size = afs->getFileCount();
	std::map<uint32_t, std::string> list;

	for (uint32_t i = 0; i < size; ++i) {
		list.insert({i, path + '/' + afs->getFilename(i)});
	}

	startWorker(Type::Export, list);
}

void MainWindow::on_actionImportFromFolder_triggered()
{
	QString path = QFileDialog::getExistingDirectory(this);

	if (path.isEmpty()) {
		return;
	}

	QStringList files = QDir(path).entryList(QDir::Files);
	uint32_t size = files.size();

	auto fileCount = afs->getFileCount();
	std::vector<std::string> afsList;
	afsList.reserve(fileCount);

	for (uint32_t i = 0; i < fileCount; ++i) {
		afsList.emplace_back(afs->getFilename(i));
	}

	std::map<uint32_t, std::string> list;

	for (uint32_t i = 0; i < size; ++i) {
		auto iter = std::find(afsList.begin(), afsList.end(), files[i].toLocal8Bit().toStdString());
		if (iter != afsList.end()) {
			uint32_t index = iter - afsList.begin();
			list.insert({index, path.toLocal8Bit().toStdString() + '/' + afsList[index]});
		}
	}

	startWorker(Type::Import, list);
}

void MainWindow::on_actionExportAFLCommon_triggered()
{
	if (afs == nullptr) {
		return; // prevent possible future errors
	}

	std::string path = QFileDialog::getSaveFileName(this, "Save AFL file", QString::fromLocal8Bit(getFilename(afs->afsName).c_str()), "AFL file (*.afl)").toLocal8Bit().toStdString();
	if (path.empty()) {
		return;
	}

	if (afs->exportAFLCommon(path)) {
		ShowInfo(this, "Success", "AFL successfully exported");
	}
	else {
		ShowError(this, "Error", "Unable to export AFL!");
	}
}

void MainWindow::on_actionImportAFLCommon_triggered()
{
	if (afs == nullptr) {
		return; // prevent possible future errors
	}

	std::string path = QFileDialog::getOpenFileName(this).toLocal8Bit().toStdString();

	if (path.empty()) {
		return;
	}

	auto result = afs->importAFLCommon(path);
	if (result == 1) {
		ShowInfo(this, "Success", "AFL successfully imported");
		drawFileList();
	}
	else {
		QString error = "Unable to import AFL";
		ShowError(this, "Error", error + (result == 2 ? " (incompatible)" : ""));
	}
}

void MainWindow::on_actionOptimize_triggered()
{
	ShowInfo(this, "Optimizer", "This option will optimize all reserved space to fit files size");

	afs->optimize();

	drawFileList();
	updateFreeSpaceLabel();

	ShowInfo(this, "Optimizer", "Operation completed!\nNow you should rebuild AFS for optimization to take effect");
}

void MainWindow::on_actionRebuild_triggered()
{
	rebuildAFS();
}

void MainWindow::on_actionAbout_triggered()
{
	AboutDialog(windowTitle(), this).exec();
}
// ---------- end menu bar ----------

// ---------- various slots ----------
void MainWindow::toAdjust_p1(bool init)
{
	auto worker = (Worker *)QObject::sender();

	worker->wait();

	// check if there is a problem
	if (worker->rsStatus != Worker::ReservedSpace::Status::Ok) {
		auto index = worker->getPosition();
		auto buttons = QMessageBox::Yes | QMessageBox::No;

		QMessageBox::StandardButton reply = QMessageBox::NoButton;

		if (init) {
			reply = ShowInfo(this, "Optimize", "Do you want to optimize reserved space (require rebuild)?", buttons);
		}
		else {
			auto errors = worker->getErrors();

			if (errors > 2) {
				buttons |= QMessageBox::NoToAll;
			}

			auto list = worker->getList();
			auto iter = list.find(index);

			QString filename = QString::fromLocal8Bit(afs->getFilename(index).c_str());
			QString sizeInfo;

			if (iter != list.end()) {
				auto fileSize = getFileSize(iter->second);
				auto rs = afs->getReservedSpace(index);
				auto reservedKB = rs.second / 1024;
				auto fileKB = fileSize / 1024;

				sizeInfo = QString("\n\nFile size: %1 KB\nReserved space: %2 KB\nOverflow: %3 KB")
							.arg(fileKB)
							.arg(reservedKB)
							.arg(fileKB - reservedKB);
			}

			reply = ShowError(this, "Not enough space",
				"Cannot import over '" + filename + "': the file is larger than the slot's reserved space." + sizeInfo +
				"\n\n⚠ WARNING for PS2/ISO modding: clicking YES will trigger a REBUILD which changes all internal AFS offsets. "
				"If you are patching an ISO, this WILL break audio and other data unless you also update the ISO filesystem. "
				"\n\nClick YES only if you are working on a standalone AFS file (not inside a PS2 ISO)."
				"\nClick NO to skip this file and keep the AFS intact.",
				buttons);
		}

		if (reply == QMessageBox::Yes) {
			auto *afs = new AFS_File(*this->afs);

			auto list = worker->getList();
			for (auto iter = list.find(index); iter != list.end(); iter++) {
				auto size = getFileSize(iter->second);

				auto rs = afs->getReservedSpace(iter->first);
				auto ors = afs->getOptimizedReservedSpace(size, AFS_File::Type::Size);

				qDebug() << "Size:" << size << "| Reserved space:" << rs.first << "| Reserved space (after rebuild):" << rs.second;

				if (size > rs.second || (worker->rsStatus.has(Worker::ReservedSpace::Status::TooMuchSpace) && rs.first > ors)) {
					afs->changeReservedSpace(iter->first, ors);
				}
			}

			oldWorker = worker; // the magic

			if (!rebuildAFS(afs)) {
				oldWorker = nullptr;
				emit done();
			}
		}
		else {
			if (worker->rsStatus.has(Worker::ReservedSpace::Status::TooMuchSpace)) {
				worker->rsStatus.remove(Worker::ReservedSpace::Status::TooMuchSpace);
				worker->start();
			}
			else {
				if (reply == QMessageBox::NoToAll) {
					worker->setSkipAll(true);
				}
				worker->skipFile();
			}
		}
	}
}

void MainWindow::toAdjust_p2(std::string path)
{
	if (oldWorker != nullptr) {
		auto *afs = this->afs;
		openAFS(path);
		if (afs != this->afs) {
			oldWorker->updateAFS(this->afs);
			oldWorker->start();
		}
		else {
			emit done();
		}
		oldWorker = nullptr;
	}
}

void MainWindow::updateFreeSpaceLabel()
{
	auto fileCount = afs->getFileCount();

	uint64_t freeSpace = afs->getFileInfo(0).address - afs->getOptimizedReservedSpace(16 + 8 * fileCount, AFS_File::Type::Size);
	uint64_t freeSpaceRebuild = 0;


	for (uint32_t i = 0; i <= fileCount; ++i) {
		auto rs = afs->getReservedSpace(i);
		auto ors = afs->getOptimizedReservedSpace(i);

		if (rs.first > ors) {
			freeSpace += (rs.first - ors);
		}
		if (rs.second > ors) {
			freeSpaceRebuild += (rs.second - ors);
		}
	}

	ui->freeSpace->setText(("Free space: " + getSize(freeSpace) + " (" + getSize(freeSpaceRebuild) + ")").c_str());
}

void MainWindow::abort()
{
	auto worker = (Worker *)QObject::sender();

	worker->wait();

	QString type = worker->type == Type::Import ? " import " : (worker->type == Type::Export ? " export" : (worker->type == Type::Rebuild ? " rebuild" : ""));

	if (ShowWarning(this, "Abort", "Are you sure that you want to abort" + type + "?", QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
		oldWorker = nullptr;
		emit done();
	}
	else {
		worker->start();
	}
}

void MainWindow::errorFile()
{
	auto worker = (Worker *)QObject::sender();

	worker->wait();

	auto index = worker->getPosition();
	auto errors = worker->getErrors();

	auto buttons = QMessageBox::Yes | QMessageBox::No;

	if (errors > 2) {
		buttons |= QMessageBox::NoToAll;
	}

	QMessageBox::StandardButton reply = QMessageBox::No;

	if (worker->type == Type::Export) {
		reply = ShowError(this, "Error", "Error while extracting '" + QString::fromLocal8Bit(afs->getFilename(index).c_str()) + "'...\nDo you to want to retry?", buttons);
	}
	else if (worker->type == Type::Import) {
		reply = ShowError(this, "Error", "Error while importing over '" + QString::fromLocal8Bit(afs->getFilename(index).c_str()) + "'...\nDo you to want to retry?", buttons);
	}
	else if (worker->type == Type::Rebuild) {
		reply = ShowError(this, "Error", "Error while rebuilding AFS file...\nDo you to want to retry?", buttons);
	}

	if (reply == QMessageBox::Yes) {
		worker->start();
	}
	else {
		if (worker->type == Type::Rebuild) {
			oldWorker = nullptr;
			emit done();
		}
		else {
			if (reply == QMessageBox::NoToAll) {
				worker->setSkipAll(true);
			}
			emit skipFile();
		}
	}
}

void MainWindow::errorMessage(const std::string &message)
{
	ShowError(this, "Error", QString::fromLocal8Bit(message.c_str()));
	emit skipFile();
}

void MainWindow::refreshRow(uint32_t index)
{
	// mark the ISO as having unsaved changes
	if (!currentISOPath.empty()) {
		isoModified = true;
	}

	auto list = ui->tableWidget->findItems(QString::number(index + 1), Qt::MatchExactly);

	int row = -1;

	for (auto item : list) {
		if (item->column() == columnID::number) {
			row = item->row();
			break;
		}
	}

	auto fileCount = afs->getFileCount();

	if (getIndexFromRow(row) == fileCount) {
		openAFS(afs->afsName);
	}
	else if (row != -1) {
		auto fileInfo = afs->getFileInfo(index);
		auto fileDesc = (index != fileCount ? afs->getFileDesc(index) : AFS_File::FileDesc());

		// size
		auto item = ui->tableWidget->item(row, columnID::size);
		//item->setText(QString::number(fileInfo.size));
		item->setText(getSize(fileInfo.size).c_str());

		auto hasOverSpace = afs->hasOverSpace(index);

		// reservedSpace
		item = ui->tableWidget->item(row, columnID::reservedSpace);
		//item->setText(QString::number(fileInfo.reservedSpace));
		item->setText(getSize(fileInfo.reservedSpace).c_str());
		if (!hasOverSpace.first) {
			item->setTextColor(Qt::GlobalColor::green);
		}
		else {
			if (fileInfo.size > fileInfo.reservedSpace) {
				item->setTextColor(Qt::GlobalColor::red);
			}
			else {
				item->setTextColor(Qt::GlobalColor::black);
			}
		}

		// afterRebuild
		item = ui->tableWidget->item(row, columnID::afterRebuild);
		//item->setText(QString::number(fileInfo.reservedSpaceRebuild));
		item->setText(getSize(fileInfo.reservedSpaceRebuild).c_str());
		if (!hasOverSpace.second) {
			item->setTextColor(Qt::GlobalColor::green);
		}
		else {
			if (fileInfo.size > fileInfo.reservedSpaceRebuild) {
				item->setTextColor(Qt::GlobalColor::red);
			}
			else {
				item->setTextColor(Qt::GlobalColor::black);
			}
		}

		// date
		auto dateItem = ui->tableWidget->item(row, columnID::dateModified);
		QDate date(fileDesc.year, fileDesc.month, fileDesc.day);
		QTime time(fileDesc.hour, fileDesc.min, fileDesc.sec);
		dateItem->setData(Qt::EditRole, QDateTime(date, time));
		dateItem->setData(Qt::DisplayRole, QDateTime(date, time));
	}
}

void MainWindow::on_actionExportSelection_triggered()
{
	if (afs == nullptr) {
		return; // prevent possible future errors
	}

	auto listIndex = getSelectedIndexes();
	uint32_t size = listIndex.size();

	std::map<uint32_t, std::string> list;

	std::string path;
	if (size == 1) {
		path = QFileDialog::getSaveFileName(this, "Save file", QString::fromLocal8Bit(afs->getFilename(listIndex[0]).c_str()), "File (*)").toLocal8Bit().toStdString();

		if (path.empty()) {
			return;
		}

		list.insert({listIndex[0], path});
	}
	else if (size > 1) {
		path = QFileDialog::getExistingDirectory(this).toLocal8Bit().toStdString();

		if (path.empty()) {
			return;
		}

		for (uint32_t i = 0; i < size; ++i) {
			list.insert({listIndex[i], path + '/' + afs->getFilename(listIndex[i])});
		}
	}

	startWorker(Type::Export, list);
}

void MainWindow::on_actionImportFile_triggered()
{
	if (afs == nullptr) {
		return; // prevent possible future errors
	}

	auto listIndex = getSelectedIndexes();

	if (listIndex.size() == 1) {
		std::string path = QFileDialog::getOpenFileName(this).toLocal8Bit().toStdString();

		if (path.empty()) {
			return;
		}

		startWorker(Type::Import, {{listIndex[0], path}});
	}
}

void MainWindow::on_actionModifyReservedSpace_triggered()
{
	if (afs == nullptr) {
		return; // prevent possible future errors
	}

	auto listIndex = getSelectedIndexes();

	if (listIndex.size() == 1) {
		auto reservedSpace = afs->getReservedSpace(listIndex[0]);

		ReservedSpaceDialog rsd(reservedSpace.first, reservedSpace.second, this);

		auto result = rsd.exec();

		if (result) {
			if (afs->changeReservedSpace(listIndex[0], rsd.getNewReservedSpace())) {
				refreshRow(listIndex[0]);
				updateFreeSpaceLabel();
			}
			else {
				ShowError(this, "Error", "Unable to set '" + QString::number(rsd.getNewReservedSpace()) + "' has reserved space");
			}
		}
	}
}

void MainWindow::on_tableWidget_cellChanged(int row, int column)
{
	if (enableCellChanged && row < afs->getFileCount()) {
		if (column == columnID::filename) {
			auto item = ui->tableWidget->item(row, column);
			auto text = item->text();
			if (text.size() > FILENAME_SIZE) {
				text.resize(FILENAME_SIZE);
				enableCellChanged = false;
				item->setText(text);
				enableCellChanged = true;
			}
			afs->changeFilename(row, text.toLocal8Bit().toStdString().c_str());
			if (!afs->commitFileDesc()) {
				ShowError(this, "Error", "Unable to save AFS");
			}
		}
		else if (column == columnID::dateModified) {
			auto item = ui->tableWidget->item(row, column);
			auto dateTime = item->data(Qt::EditRole).toDateTime();
			auto date = dateTime.date();
			auto time = dateTime.time();

			tm tm {};
			tm.tm_year = date.year() - 1900;
			tm.tm_mon = date.month() - 1;
			tm.tm_mday = date.day();
			tm.tm_hour = time.hour();
			tm.tm_min = time.minute();
			tm.tm_sec = time.second();

			afs->changeDateTime(getIndexFromRow(row), &tm);
			if (!afs->commitFileDesc()) {
				ShowError(this, "Error", "Unable to save AFS");
			}
		}
	}
}

void MainWindow::on_tableWidget_cellDoubleClicked(int row, int column)
{
	if (enableCellChanged) {
		auto item = ui->tableWidget->item(row, column);

		if (column == columnID::reservedSpace || column == columnID::afterRebuild) {
			ui->actionModifyReservedSpace->trigger();
		}
	}
}

void MainWindow::on_tableWidget_customContextMenuRequested(QPoint pos)
{
	QMenu contextMenu;
	auto size = ui->tableWidget->selectionModel()->selectedRows().size();
	if (size >= 1) {
		contextMenu.addAction(ui->actionExportSelection);
		if (size == 1) {
			ui->actionExportSelection->setText("Export file");
			contextMenu.addAction(ui->actionImportFile);
			contextMenu.addAction(ui->actionModifyReservedSpace);
			if (ui->actionPreview->isEnabled()) {
				contextMenu.addAction(ui->actionPreview);
				contextMenu.addAction(ui->actionConvertWAV);
			}
		}
		else {
			ui->actionExportSelection->setText("Export selection");
		}
	}
	ui->actionExportSelection->setToolTip(ui->actionExportSelection->text());
	contextMenu.exec(ui->tableWidget->mapToGlobal(pos));
}

void MainWindow::updatePreviewAvailability()
{
	bool enable = false;

	if (afs != nullptr) {
		auto listIndex = getSelectedIndexes();

		if (listIndex.size() == 1 && listIndex[0] < afs->getFileCount()) {
			std::string filename = afs->getFilename(listIndex[0]);

			std::size_t dot = filename.find_last_of('.');
			if (dot != std::string::npos) {
				std::string extension = getLowercase(filename.substr(dot + 1));
				enable = (extension == "adx");
			}
		}
	}

	ui->previewButton->setEnabled(enable);
	ui->actionPreview->setEnabled(enable);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	if (isoModified && !currentISOPath.empty()) {
		auto reply = ShowInfo(this, "Unsaved changes",
			"You have modified the AFS but have not saved it to the ISO yet.\n\n"
			"If you close now, your changes will be LOST.\n\n"
			"Do you want to save to ISO before closing?",
			QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);

		if (reply == QMessageBox::Cancel) {
			event->ignore();
			return;
		}

		if (reply == QMessageBox::Yes) {
			on_actionSaveAFStoISO_triggered();
			// if save failed (isoModified still true), don't close
			if (isoModified) {
				event->ignore();
				return;
			}
		}
	}

	// clean up the temporary AFS file extracted from the ISO
	if (!tempAFSPath.empty()) {
		QFile::remove(QString::fromLocal8Bit(tempAFSPath.c_str()));
		tempAFSPath.clear();
	}

	event->accept();
}

void MainWindow::on_actionOpenISO_triggered()
{
	QString isoPath = QFileDialog::getOpenFileName(this, "Open PS2 ISO", QString(), "PS2 ISO files (*.iso *.img *.bin)");

	if (isoPath.isEmpty()) {
		return;
	}

	ShowInfo(this, "Opening ISO", "Scanning ISO filesystem...\nThis may take a moment for large ISOs.");

	ISO_File iso(isoPath.toLocal8Bit().toStdString());

	if (!iso.isOpen()) {
		auto err = iso.getError();
		QString reason;
		if (err.unableToOpen)      reason = "could not open the file";
		else if (err.notISO)       reason = "not a valid PS2 UDF ISO";
		else if (err.corruptedContent) reason = "corrupted or unrecognized UDF structure";
		else                       reason = "unknown error";

		ShowError(this, "Error", "Cannot open ISO:\n" + reason);
		return;
	}

	// Collect all .afs files found in the ISO
	const auto &files = iso.getFiles();
	QStringList afsList;
	std::vector<ISO_File::FileEntry> afsEntries;

	for (const auto &entry : files) {
		std::string lower = entry.name;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
		if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".afs") {
			afsList << QString::fromLocal8Bit(entry.fullPath.c_str()) +
						QString(" (%1 MB)").arg(static_cast<double>(entry.size) / 1024.0 / 1024.0, 0, 'f', 1);
			afsEntries.push_back(entry);
		}
	}

	if (afsList.isEmpty()) {
		ShowError(this, "Error", "No AFS files found inside the ISO.");
		return;
	}

	// Let the user pick which AFS to open
	bool ok = false;
	QString chosen = QInputDialog::getItem(this, "Select AFS", "AFS files found in ISO:", afsList, 0, false, &ok);

	if (!ok || chosen.isEmpty()) {
		return;
	}

	int chosenIndex = afsList.indexOf(chosen);
	if (chosenIndex < 0 || chosenIndex >= static_cast<int>(afsEntries.size())) {
		return;
	}

	const ISO_File::FileEntry &entry = afsEntries[static_cast<size_t>(chosenIndex)];

	// Extract the chosen AFS to a temp file
	QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	QString uniqueId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	QString baseName = QString::fromLocal8Bit(entry.name.c_str());
	QString tempAfsPath = QDir(tempDir).filePath("AFSManager_" + uniqueId + "_" + baseName);

	ISO_File::Error extractError;
	if (!iso.extractFile(entry, tempAfsPath.toLocal8Bit().toStdString(), &extractError)) {
		ShowError(this, "Error", "Failed to extract AFS from ISO.");
		return;
	}

	// Open the extracted AFS normally
	openAFS(tempAfsPath.toLocal8Bit().toStdString());

	if (this->afs == nullptr) {
		QFile::remove(tempAfsPath);
		return;
	}

	// Store ISO association so "Save AFS to ISO" knows where to write back
	currentISOPath  = isoPath.toLocal8Bit().toStdString();
	currentISOEntry = entry;
	tempAFSPath     = tempAfsPath.toLocal8Bit().toStdString();
	isoModified     = false;
	ui->actionSaveAFStoISO->setEnabled(true);

	// Show a status hint
	ui->afsName->setText(ui->afsName->text() +
		" [ISO: " + QString::fromLocal8Bit(QFileInfo(isoPath).fileName().toLocal8Bit()) + "]");
}

void MainWindow::on_actionSaveAFStoISO_triggered()
{
	if (afs == nullptr || currentISOPath.empty()) {
		return;
	}

	// Confirm with the user
	auto reply = ShowInfo(this, "Save AFS to ISO",
		"This will write the current AFS directly into the ISO at its original sector.\n"
		"The ISO file will be modified in-place — no LBA offsets will change.\n\n"
		"ISO: " + QString::fromLocal8Bit(QFileInfo(QString::fromLocal8Bit(currentISOPath.c_str())).fileName().toLocal8Bit()) + "\n"
		"AFS slot: " + QString::fromLocal8Bit(currentISOEntry.fullPath.c_str()) + "\n\n"
		"Continue?",
		QMessageBox::Yes | QMessageBox::No);

	if (reply != QMessageBox::Yes) {
		return;
	}

	// First, commit the AFS to its temp file so we have the latest bytes on disk
	// (importFile already writes directly to the AFS file on disk, so afs->afsName
	// is always up-to-date — we just need to write that file back into the ISO)
	ISO_File iso(currentISOPath);

	if (!iso.isOpen()) {
		ShowError(this, "Error", "Cannot open ISO for writing.");
		return;
	}

	ISO_File::Error writeError;
	bool ok = iso.replaceFile(currentISOEntry, afs->afsName, &writeError);

	if (!ok) {
		QString reason;
		if (writeError.fileTooLarge) {
			uint64_t maxMB = (((currentISOEntry.size + 2047) / 2048) * 2048) / 1024 / 1024;
			reason = QString("The modified AFS is larger than the original slot in the ISO.\n"
							 "Maximum allowed size: %1 MB.\n\n"
							 "To fit a larger AFS you would need to rebuild the ISO — "
							 "but that would change LBAs and break the game.\n"
							 "Try keeping your replacements within the existing reserved space of each slot.").arg(maxMB);
		}
		else if (writeError.writeFailed) {
			reason = "Write failed (is the ISO read-only or in use by another program?)";
		}
		else {
			reason = "Unknown error";
		}

		ShowError(this, "Error", "Could not save AFS to ISO:\n\n" + reason);
		return;
	}

	ShowInfo(this, "Success",
		"AFS saved to ISO successfully!\n\n"
		"The ISO has been patched in-place. Audio and all other data should work correctly.\n"
		"No rebuild was needed — all LBA offsets are unchanged.");

	isoModified = false;
}

void MainWindow::applySearchFilter(const QString &text)
{
	QString needle = text.trimmed();

	for (int row = 0; row < ui->tableWidget->rowCount(); ++row) {
		bool visible = true;

		if (!needle.isEmpty()) {
			auto item = ui->tableWidget->item(row, columnID::filename);
			visible = (item != nullptr) && item->text().contains(needle, Qt::CaseInsensitive);
		}

		ui->tableWidget->setRowHidden(row, !visible);
	}
}

void MainWindow::on_actionPreview_triggered()
{
	if (afs == nullptr) {
		return; // prevent possible future errors
	}

	auto listIndex = getSelectedIndexes();

	if (listIndex.size() != 1 || listIndex[0] >= afs->getFileCount()) {
		return;
	}

	uint32_t index = listIndex[0];
	std::string filename = afs->getFilename(index);

	if (!adxPreview->play(afs, index, filename)) {
		// error already reported through AdxPreview::errorMessage
	}
}

void MainWindow::on_actionConvertWAV_triggered()
{
	if (afs == nullptr) {
		return; // prevent possible future errors
	}

	auto listIndex = getSelectedIndexes();

	if (listIndex.size() != 1 || listIndex[0] >= afs->getFileCount()) {
		return;
	}

	uint32_t index = listIndex[0];

	QString wavPath = QFileDialog::getOpenFileName(this, "Select WAV file", QString(), "WAV files (*.wav)");

	if (wavPath.isEmpty()) {
		return;
	}

	// encode to a temporary ADX file, then reuse the existing Import flow to
	// write it into the AFS slot (handles reserved space, refresh, etc.)
	QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	if (tempDir.isEmpty()) {
		tempDir = QDir::tempPath();
	}

	QString uniqueId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	QString tempAdxPath = QDir(tempDir).filePath("AFSManager_" + uniqueId + "_converted.adx");

	ADX_File::Error encodeError;
	bool ok = ADX_File::encodeFromWAV(wavPath.toLocal8Bit().toStdString(), tempAdxPath.toLocal8Bit().toStdString(), 500, &encodeError);

	if (!ok) {
		QString reason;
		if (encodeError.unableToOpen) {
			reason = "unable to open the selected WAV file";
		}
		else if (encodeError.notWAV) {
			reason = "the selected file is not a valid WAV file";
		}
		else if (encodeError.unsupportedWAV) {
			reason = "unsupported WAV format (only 16-bit PCM mono/stereo is supported)";
		}
		else if (encodeError.writeFailed) {
			reason = "unable to write the converted ADX file";
		}
		else {
			reason = "unknown conversion error";
		}

		ShowError(this, "Error", "Unable to convert '" + QFileInfo(wavPath).fileName() + "':\n" + reason);
		return;
	}

	std::string tempAdxPathStd = tempAdxPath.toLocal8Bit().toStdString();

	startWorker(Type::Import, {{index, tempAdxPathStd}}, nullptr, tempAdxPath);
}
// ---------- end various slots ----------

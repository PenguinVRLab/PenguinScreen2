// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GameListModel.h"
#include "GameListRefreshThread.h"
#include "GameListWidget.h"
#include "QtHost.h"
#include "QtUtils.h"

#include "Settings/InterfaceSettingsWidget.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <QtCore/QSortFilterProxyModel>
#include <QtCore/QDir>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPalette>
#include <QtGui/QPixmap>
#include <QtGui/QPixmapCache>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStyledItemDelegate>
#include <QShortcut>

static const char* SUPPORTED_FORMATS_STRING = QT_TRANSLATE_NOOP(GameListWidget,
	".bin/.iso (ISO Disc Images)\n"
	".mdf (Media Descriptor File)\n"
	".chd (Compressed Hunks of Data)\n"
	".cso (Compressed ISO)\n"
	".zso (Compressed ISO)\n"
	".gz (Gzip Compressed ISO)");

static constexpr float MIN_SCALE = 0.1f;
static constexpr float MAX_SCALE = 2.0f;

static constexpr GameListModel::Column DEFAULT_SORT_COLUMN = GameListModel::Column_Title;
static constexpr int DEFAULT_SORT_INDEX = static_cast<int>(DEFAULT_SORT_COLUMN);
static constexpr Qt::SortOrder DEFAULT_SORT_ORDER = Qt::AscendingOrder;

static constexpr std::array<int, GameListModel::Column_Count> DEFAULT_COLUMN_WIDTHS = {{
	55,
	85,
	-1,
	-1,
	75,
	95,
	90,
	80,
	60,
	120
}};
static_assert(static_cast<int>(DEFAULT_COLUMN_WIDTHS.size()) <= GameListModel::Column_Count,
	"Game List: More default column widths than column types.");

class GameListSortModel final : public QSortFilterProxyModel
{
public:
	explicit GameListSortModel(GameListModel* parent)
		: QSortFilterProxyModel(parent)
		, m_model(parent)
	{
	}

	void setFilterType(GameList::EntryType type)
	{
		beginFilterChange();
		m_filter_type = type;
		endFilterChange(Direction::Rows);
	}
	void setFilterRegion(GameList::Region region)
	{
		beginFilterChange();
		m_filter_region = region;
		endFilterChange(Direction::Rows);
	}
	void setFilterName(const QString& name)
	{
		beginFilterChange();
		m_filter_name = name;
		endFilterChange(Direction::Rows);
	}

	bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override
	{
		if (m_filter_type != GameList::EntryType::Count ||
			m_filter_region != GameList::Region::Count ||
			!m_filter_name.isEmpty())
		{
			const auto lock = GameList::GetLock();
			const GameList::Entry* entry = GameList::GetEntryByIndex(source_row);
			if (m_filter_type != GameList::EntryType::Count && entry->type != m_filter_type)
				return false;
			if (m_filter_region != GameList::Region::Count && entry->region != m_filter_region)
				return false;
			if (!m_filter_name.isEmpty() &&
				!QString::fromStdString(entry->path).contains(m_filter_name, Qt::CaseInsensitive) &&
				!QString::fromStdString(entry->serial).contains(m_filter_name, Qt::CaseInsensitive) &&
				!QString::fromStdString(entry->title).contains(m_filter_name, Qt::CaseInsensitive) &&
				!QString::fromStdString(entry->title_en).contains(m_filter_name, Qt::CaseInsensitive))
				return false;
		}

		return QSortFilterProxyModel::filterAcceptsRow(source_row, source_parent);
	}

	bool lessThan(const QModelIndex& source_left, const QModelIndex& source_right) const override
	{
		return m_model->lessThan(source_left, source_right, source_left.column());
	}

private:
	GameListModel* m_model;
	GameList::EntryType m_filter_type = GameList::EntryType::Count;
	GameList::Region m_filter_region = GameList::Region::Count;
	QString m_filter_name;
};

namespace
{
	class GameListIconStyleDelegate final : public QStyledItemDelegate
	{
	public:
		GameListIconStyleDelegate(QWidget* parent)
			: QStyledItemDelegate(parent)
		{
		}
		~GameListIconStyleDelegate() = default;

		void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
		{
			Q_ASSERT(index.isValid());

			QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &option, painter, option.widget);

			const QPixmap icon = qvariant_cast<QPixmap>(index.data(Qt::DecorationRole));

			if (icon.isNull())
				return;

			painter->save();

			const QRect rect = option.rect;
			painter->setClipRect(rect);

			const int icon_width = static_cast<int>(static_cast<qreal>(icon.width()) / icon.devicePixelRatio());
			const int icon_height = static_cast<int>(static_cast<qreal>(icon.height()) / icon.devicePixelRatio());
			const QPoint icon_top_left = QPoint((rect.width() - icon_width) / 2, (rect.height() - icon_height) / 2);

			if (option.state & QStyle::State_Selected)
			{
				const bool enabled = option.state & QStyle::State_Enabled;
				QColor color = option.palette.color(enabled ? QPalette::Normal : QPalette::Disabled, QPalette::Highlight);
				color.setAlphaF(0.3f);

				const QString key = QString::fromStdString(fmt::format("{:016X}-{:d}-{:08X}", icon.cacheKey(), enabled, color.rgba()));
				QPixmap highlighted_icon;
				if (!QPixmapCache::find(key, &highlighted_icon))
				{
					QImage img = icon.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);

					QPainter tinted_painter(&img);
					tinted_painter.setCompositionMode(QPainter::CompositionMode_SourceAtop);
					tinted_painter.fillRect(0, 0, img.width(), img.height(), color);
					tinted_painter.end();

					highlighted_icon = QPixmap(QPixmap::fromImage(img));
					QPixmapCache::insert(key, highlighted_icon);
				}

				painter->drawPixmap(rect.topLeft() + icon_top_left, highlighted_icon);
			}
			else if (index.column() == GameListModel::Column_Type)
			{
				const QColor color = option.palette.color(QPalette::Text);
				const QString key = QString::fromStdString(fmt::format("type-{:016X}-{:08X}", icon.cacheKey(), color.rgba()));

				QPixmap tinted_icon;
				if (!QPixmapCache::find(key, &tinted_icon))
				{
					QImage img = icon.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);

					QPainter tinted_painter(&img);
					tinted_painter.setCompositionMode(QPainter::CompositionMode_SourceAtop);
					tinted_painter.fillRect(0, 0, img.width(), img.height(), color);
					tinted_painter.end();

					tinted_icon = QPixmap(QPixmap::fromImage(img));
					QPixmapCache::insert(key, tinted_icon);
				}

				painter->drawPixmap(rect.topLeft() + icon_top_left, tinted_icon);
			}
			else
			{
				painter->drawPixmap(rect.topLeft() + icon_top_left, icon);
			}

			painter->restore();
		}
	};
}

GameListWidget::GameListWidget(QWidget* parent )
	: QWidget(parent)
{
}

GameListWidget::~GameListWidget() = default;

void GameListWidget::initialize()
{
	const float cover_scale = Host::GetBaseFloatSettingValue("UI", "GameListCoverArtScale", 0.45f);
	const bool show_cover_titles = Host::GetBaseBoolSettingValue("UI", "GameListShowCoverTitles", true);
	const bool show_full_cover_titles = Host::GetBaseBoolSettingValue("UI", "GameListShowFullCoverTitles", true);
	m_model = new GameListModel(cover_scale, show_cover_titles, show_full_cover_titles, devicePixelRatioF(), this);
	m_model->updateCacheSize(width(), height());

	m_sort_model = new GameListSortModel(m_model);
	m_sort_model->setSourceModel(m_model);

	m_ui.setupUi(this);
	m_ui.stack->installEventFilter(this);
	m_ui.stack->setAutoFillBackground(false);

	for (u32 type = 0; type < static_cast<u32>(GameList::EntryType::Count); type++)
	{
		if (type != static_cast<u32>(GameList::EntryType::Invalid))
		{
			m_ui.filterType->addItem(GameListModel::getIconForType(static_cast<GameList::EntryType>(type)),
				GameList::EntryTypeToString(static_cast<GameList::EntryType>(type), true));
		}
	}

	for (u32 region = 0; region < static_cast<u32>(GameList::Region::Count); region++)
	{
		m_ui.filterRegion->addItem(GameListModel::getIconForRegion(static_cast<GameList::Region>(region)),
			GameList::RegionToString(static_cast<GameList::Region>(region), true));
	}

	connect(m_ui.viewGameList, &QPushButton::clicked, this, &GameListWidget::showGameList);
	connect(m_ui.viewGameGrid, &QPushButton::clicked, this, &GameListWidget::showGameGrid);
	connect(m_ui.gridScale, &QSlider::valueChanged, this, &GameListWidget::gridIntScale);
	connect(m_ui.viewGridTitles, &QPushButton::toggled, this, &GameListWidget::setShowCoverTitles);
	connect(m_ui.viewFullGridTitles, &QPushButton::toggled, this, &GameListWidget::setShowFullCoverTitles);
	connect(m_ui.filterType, &QComboBox::currentIndexChanged, this, [this](int index) {
		m_sort_model->setFilterType((index == 0) ? GameList::EntryType::Count : static_cast<GameList::EntryType>(index - 1));
	});
	connect(m_ui.filterRegion, &QComboBox::currentIndexChanged, this, [this](int index) {
		m_sort_model->setFilterRegion((index == 0) ? GameList::Region::Count : static_cast<GameList::Region>(index - 1));
	});
	connect(m_ui.searchText, &QLineEdit::textChanged, this, [this](const QString& text) {
		m_sort_model->setFilterName(text);
	});

	connect(new QShortcut(QKeySequence::Find, this), &QShortcut::activated, [this]() {
		m_ui.searchText->setFocus();
	});

	m_table_view = new QTableView(m_ui.stack);
	m_table_view->setModel(m_sort_model);
	m_table_view->setSelectionMode(QAbstractItemView::SingleSelection);
	m_table_view->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table_view->setContextMenuPolicy(Qt::CustomContextMenu);
	m_table_view->setAlternatingRowColors(true);
	m_table_view->setMouseTracking(true);
	m_table_view->setShowGrid(false);
	m_table_view->setCurrentIndex(QModelIndex());
	m_table_view->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
	m_table_view->horizontalHeader()->setHighlightSections(false);
	m_table_view->horizontalHeader()->setSectionsMovable(true);
	m_table_view->verticalHeader()->hide();
	m_table_view->setVerticalScrollMode(QAbstractItemView::ScrollMode::ScrollPerPixel);

	m_table_view->setItemDelegateForColumn(0, new GameListIconStyleDelegate(this));
	m_table_view->setItemDelegateForColumn(8, new GameListIconStyleDelegate(this));
	m_table_view->setItemDelegateForColumn(9, new GameListIconStyleDelegate(this));

	connect(m_table_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
		&GameListWidget::onSelectionModelCurrentChanged);
	connect(m_table_view, &QTableView::activated, this, &GameListWidget::onTableViewItemActivated);
	connect(m_table_view, &QTableView::customContextMenuRequested, this,
		&GameListWidget::onTableViewContextMenuRequested);
	connect(m_table_view->horizontalHeader(), &QHeaderView::customContextMenuRequested, this,
		&GameListWidget::onTableViewHeaderContextMenuRequested);

	connect(m_table_view->horizontalHeader(), &QHeaderView::sectionMoved, this, &GameListWidget::onTableHeaderStateChanged);
	connect(m_table_view->horizontalHeader(), &QHeaderView::sectionResized, this, &GameListWidget::onTableHeaderStateChanged);
	connect(m_table_view->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this,
		[this](const int column, const Qt::SortOrder sort_order) { GameListWidget::saveSortSettings(column, sort_order); GameListWidget::onTableHeaderStateChanged(); });

	if (Host::ContainsBaseSettingValue("GameListTableView", "HeaderState"))
	{
		loadTableHeaderState();
	}
	else
	{
		applyTableHeaderDefaults();
	}

	m_table_view->setSortingEnabled(true);

	m_table_view->horizontalHeader()->show();
	resizeTableViewColumnsToFit();

	m_ui.stack->insertWidget(0, m_table_view);

	m_list_view = new GameListGridListView(m_ui.stack);
	m_list_view->setModel(m_sort_model);
	m_list_view->setModelColumn(GameListModel::Column_Cover);
	m_list_view->setSelectionMode(QAbstractItemView::SingleSelection);
	m_list_view->setViewMode(QListView::IconMode);
	m_list_view->setResizeMode(QListView::Adjust);
	m_list_view->setUniformItemSizes(true);
	m_list_view->setItemAlignment(Qt::AlignHCenter);
	m_list_view->setContextMenuPolicy(Qt::CustomContextMenu);
	m_list_view->setFrameStyle(QFrame::NoFrame);
	m_list_view->setVerticalScrollMode(QAbstractItemView::ScrollMode::ScrollPerPixel);
	m_list_view->verticalScrollBar()->setSingleStep(15);
	m_list_view->setWordWrap(show_full_cover_titles);
	onCoverScaleChanged();

	connect(m_list_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
		&GameListWidget::onSelectionModelCurrentChanged);
	connect(m_list_view, &GameListGridListView::zoomIn, this, &GameListWidget::gridZoomIn);
	connect(m_list_view, &GameListGridListView::zoomOut, this, &GameListWidget::gridZoomOut);
	connect(m_list_view, &QListView::activated, this, &GameListWidget::onListViewItemActivated);
	connect(m_list_view, &QListView::customContextMenuRequested, this, &GameListWidget::onListViewContextMenuRequested);
	connect(m_model, &GameListModel::coverScaleChanged, this, &GameListWidget::onCoverScaleChanged);

	m_ui.stack->insertWidget(1, m_list_view);

	m_empty_widget = new QWidget(m_ui.stack);
	m_empty_ui.setupUi(m_empty_widget);
	m_empty_ui.supportedFormats->setText(qApp->translate("GameListWidget", SUPPORTED_FORMATS_STRING));
	connect(m_empty_ui.addGameDirectory, &QPushButton::clicked, this, [this]() { emit addGameDirectoryRequested(); });
	connect(m_empty_ui.scanForNewGames, &QPushButton::clicked, this, [this]() { refresh(false, true); });
	connect(qApp, &QGuiApplication::applicationStateChanged, this, [this]() { GameListWidget::updateCustomBackgroundState(); });
	m_ui.stack->insertWidget(2, m_empty_widget);

	if (Host::GetBaseBoolSettingValue("UI", "GameListGridView", false))
		m_ui.stack->setCurrentIndex(1);
	else
		m_ui.stack->setCurrentIndex(0);

	setFocusProxy(m_ui.stack->currentWidget());

	updateToolbar();
	resizeTableViewColumnsToFit();
	setCustomBackground();
}

void GameListWidget::setCustomBackground()
{
	if (m_background_movie != nullptr)
	{
		m_background_movie->disconnect(this);
		delete m_background_movie;
		m_background_movie = nullptr;
	}

	std::string path = Host::GetBaseStringSettingValue("UI", "GameListBackgroundPath");
	if (!Path::IsAbsolute(path))
		path = Path::Combine(EmuFolders::DataRoot, path);

	if (!path.empty() && FileSystem::FileExists(path.c_str()))
	{
		QString img_path = QString::fromStdString(path);
		const QByteArray format = (img_path.endsWith(".png", Qt::CaseInsensitive)) ? QByteArray("apng") : QByteArray();
		m_background_movie = new QMovie(img_path, format, this);
		if (!m_background_movie->isValid())
		{
			Console.Warning("Failed to load background movie from: %s", path.c_str());
			delete m_background_movie;
			m_background_movie = nullptr;
		}
		else if (const s64 file_size = FileSystem::GetPathFileSize(path.c_str()); file_size > 0 && file_size < 64 * 1024 * 1024)
			m_background_movie->setCacheMode(QMovie::CacheAll);
	}

	m_background_last_size = QSize();

	if (!m_background_movie)
	{
		m_background_pixmap = QPixmap();
		m_ui.stack->setAutoFillBackground(true);
		m_table_view->viewport()->setAutoFillBackground(true);
		m_list_view->viewport()->setAutoFillBackground(true);

		m_ui.stack->setPalette(QPalette());
		m_background_text_color = QColor();
		m_empty_widget->setPalette(QPalette());
		m_empty_widget->setAutoFillBackground(false);

		m_ui.stack->update();
		m_table_view->setAlternatingRowColors(true);
		return;
	}

	m_background_scaling = QtUtils::ScalingMode::Fit;
	const std::string ar_value = Host::GetBaseStringSettingValue("UI", "GameListBackgroundMode", InterfaceSettingsWidget::BACKGROUND_SCALE_NAMES[static_cast<u8>(QtUtils::ScalingMode::Fit)]);
	for (u8 i = 0; i < static_cast<u8>(QtUtils::ScalingMode::MaxCount); i++)
	{
		if (InterfaceSettingsWidget::BACKGROUND_SCALE_NAMES[i] != nullptr)
		{
			if (ar_value == InterfaceSettingsWidget::BACKGROUND_SCALE_NAMES[i])
			{
				m_background_scaling = static_cast<QtUtils::ScalingMode>(i);
				break;
			}
		}
	}

	m_background_opacity = Host::GetBaseFloatSettingValue("UI", "GameListBackgroundOpacity", 100.0f);

	connect(m_background_movie, &QMovie::frameChanged, this, &GameListWidget::processBackgroundFrames);
	m_ui.stack->setAutoFillBackground(false);

	m_table_view->viewport()->setAutoFillBackground(false);
	m_list_view->viewport()->setAutoFillBackground(false);
	m_background_movie->start();
	updateCustomBackgroundState();
	m_table_view->setAlternatingRowColors(false);
	processBackgroundFrames();
}

void GameListWidget::updateCustomBackgroundState()
{
	if (m_background_movie && m_background_movie->isValid())
		m_background_movie->setPaused(!(isVisible() && qGuiApp->applicationState() == Qt::ApplicationActive));
}

void GameListWidget::processBackgroundFrames()
{
	if (!m_background_movie || !m_background_movie->isValid() || !isVisible())
		return;

	const QSize widget_size(m_ui.stack->width(), m_ui.stack->height());
	if (widget_size.isEmpty())
		return;

	const int frame_number = m_background_movie->currentFrameNumber();
	const qreal dpr = devicePixelRatioF();

	if (frame_number == m_background_last_frame && widget_size == m_background_last_size && qFuzzyCompare(dpr, m_background_last_dpr))
		return;

	QPixmap pm = m_background_movie->currentPixmap();
	updateBackgroundTextColor(pm);
	QtUtils::resizeAndScalePixmap(&pm, widget_size.width(), widget_size.height(), dpr, m_background_scaling, m_background_opacity);

	m_background_pixmap = std::move(pm);
	m_background_last_frame = frame_number;
	m_background_last_size = widget_size;
	m_background_last_dpr = dpr;
	m_ui.stack->update();
}

void GameListWidget::updateBackgroundTextColor(const QPixmap& frame)
{
	if (frame.isNull())
		return;

	const QImage sampled = frame.scaled(32, 32, Qt::IgnoreAspectRatio, Qt::FastTransformation).toImage();
	const QColor average = sampled.scaled(1, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).pixelColor(0, 0);
	const QColor base = qApp->palette().color(QPalette::Base);
	const qreal coverage = average.alphaF() * std::clamp(m_background_opacity / 100.0f, 0.0f, 1.0f);
	const qreal brightness = qGray(average.rgb()) * coverage + qGray(base.rgb()) * (1.0 - coverage);
	const QColor text_color = (brightness > 127.5) ? Qt::black : Qt::white;

	if (m_background_text_color == text_color)
		return;
	m_background_text_color = text_color;

	QColor highlight_color = qApp->palette().color(QPalette::Highlight);
	highlight_color.setAlpha(128);
	const QColor empty_backdrop_color = (text_color == Qt::black) ? QColor(255, 255, 255, 128) : QColor(0, 0, 0, 128);

	QPalette palette;
	palette.setColor(QPalette::Text, text_color);
	palette.setColor(QPalette::WindowText, text_color);
	palette.setColor(QPalette::Highlight, highlight_color);
	m_ui.stack->setPalette(palette);

	QPalette empty_palette;
	empty_palette.setColor(QPalette::Window, empty_backdrop_color);
	m_empty_widget->setPalette(empty_palette);
	m_empty_widget->setAutoFillBackground(true);
}

bool GameListWidget::isShowingGameList() const
{
	return m_ui.stack->currentIndex() == 0;
}

bool GameListWidget::isShowingGameGrid() const
{
	return m_ui.stack->currentIndex() == 1;
}

bool GameListWidget::getShowGridCoverTitles() const
{
	return m_model->getShowCoverTitles();
}

bool GameListWidget::getShowGridFullCoverTitles() const
{
	return m_model->getShowFullCoverTitles();
}

void GameListWidget::refresh(bool invalidate_cache, bool popup_on_error)
{
	cancelRefresh();

	m_refresh_thread = new GameListRefreshThread(invalidate_cache, popup_on_error);
	connect(m_refresh_thread, &GameListRefreshThread::refreshProgress, this, &GameListWidget::onRefreshProgress,
		Qt::QueuedConnection);
	connect(m_refresh_thread, &GameListRefreshThread::refreshComplete, this, &GameListWidget::onRefreshComplete,
		Qt::QueuedConnection);
	m_refresh_thread->start();
}

void GameListWidget::cancelRefresh()
{
	if (!m_refresh_thread)
		return;

	m_refresh_thread->cancel();
	m_refresh_thread->wait();

	while (m_refresh_thread)
		QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 1);
}

void GameListWidget::reloadThemeSpecificImages()
{
	m_model->reloadThemeSpecificImages();
	m_background_last_size = QSize();
	processBackgroundFrames();
}

void GameListWidget::onRefreshProgress(const QString& status, int current, int total)
{
	if (m_ui.stack->currentIndex() == 2)
	{
		m_ui.stack->setCurrentIndex(Host::GetBaseBoolSettingValue("UI", "GameListGridView", false) ? 1 : 0);
		setFocusProxy(m_ui.stack->currentWidget());
	}

	m_model->refresh();
	emit refreshProgress(status, current, total);
}

void GameListWidget::onRefreshComplete()
{
	m_model->refresh();
	emit refreshComplete();

	pxAssertRel(m_refresh_thread, "Has a refresh thread");
	m_refresh_thread->wait();
	delete m_refresh_thread;
	m_refresh_thread = nullptr;

	if (m_model->rowCount() == 0)
	{
		m_ui.stack->setCurrentIndex(2);
		setFocusProxy(nullptr);
	}
}

void GameListWidget::onSelectionModelCurrentChanged(const QModelIndex& current, const QModelIndex& previous)
{
	const QModelIndex source_index = m_sort_model->mapToSource(current);
	if (!source_index.isValid() || source_index.row() >= static_cast<int>(GameList::GetEntryCount()))
		return;

	emit selectionChanged();
}

void GameListWidget::onTableViewItemActivated(const QModelIndex& index)
{
	const QModelIndex source_index = m_sort_model->mapToSource(index);
	if (!source_index.isValid() || source_index.row() >= static_cast<int>(GameList::GetEntryCount()))
		return;

	emit entryActivated();
}

void GameListWidget::onTableViewContextMenuRequested(const QPoint& point)
{
	emit entryContextMenuRequested(m_table_view->mapToGlobal(point));
}

void GameListWidget::onListViewItemActivated(const QModelIndex& index)
{
	const QModelIndex source_index = m_sort_model->mapToSource(index);
	if (!source_index.isValid() || source_index.row() >= static_cast<int>(GameList::GetEntryCount()))
		return;

	emit entryActivated();
}

void GameListWidget::onListViewContextMenuRequested(const QPoint& point)
{
	emit entryContextMenuRequested(m_list_view->mapToGlobal(point));
}

void GameListWidget::onTableViewHeaderContextMenuRequested(const QPoint& point)
{
	QHeaderView* header = m_table_view->horizontalHeader();

	QMenu* menu = new QMenu(this);
	menu->setAttribute(Qt::WA_DeleteOnClose);

	for (int column = 0; column < GameListModel::Column_Count; column++)
	{
		if (column == GameListModel::Column_Cover)
			continue;
		const QString title = m_model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
		QAction* const action = menu->addAction(title);
		action->setCheckable(true);
		action->setChecked(!header->isSectionHidden(column));
		connect(action, &QAction::triggered, [this, header, column, action]() {
			header->setSectionHidden(column, !action->isChecked());
			ensureMinimumOneColumnVisible();
			onTableHeaderStateChanged();
		});
	}

	menu->addSeparator();

	menu->addAction(tr("Reset All Columns"), this, &GameListWidget::resetTableHeaderToDefault);

	menu->popup(header->mapToGlobal(point));
}

void GameListWidget::onCoverScaleChanged()
{
	m_model->updateCacheSize(width(), height());

	m_list_view->setSpacing(m_model->getCoverArtSpacing());

	QFont font;
	font.setPointSizeF(20.0f * m_model->getCoverScale());
	m_list_view->setFont(font);
}

void GameListWidget::listZoom(float delta)
{
	const float new_scale = std::clamp(m_model->getCoverScale() + delta, MIN_SCALE, MAX_SCALE);
	Host::SetBaseFloatSettingValue("UI", "GameListCoverArtScale", new_scale);
	Host::CommitBaseSettingChanges();
	m_model->setCoverScale(new_scale);
	updateToolbar();
}

void GameListWidget::gridZoomIn()
{
	listZoom(0.05f);
}

void GameListWidget::gridZoomOut()
{
	listZoom(-0.05f);
}

void GameListWidget::gridIntScale(int int_scale)
{
	const float new_scale = std::clamp(static_cast<float>(int_scale) / 100.0f, MIN_SCALE, MAX_SCALE);

	Host::SetBaseFloatSettingValue("UI", "GameListCoverArtScale", new_scale);
	Host::CommitBaseSettingChanges();
	m_model->setCoverScale(new_scale);
	updateToolbar();
}

void GameListWidget::refreshGridCovers()
{
	m_model->refreshCovers();
}

void GameListWidget::showGameList()
{
	Host::SetBaseBoolSettingValue("UI", "GameListGridView", false);
	Host::CommitBaseSettingChanges();

	if (m_ui.stack->currentIndex() == 0 || m_model->rowCount() == 0)
	{
		updateToolbar();
		return;
	}

	m_ui.stack->setCurrentIndex(0);
	setFocusProxy(m_ui.stack->currentWidget());
	resizeTableViewColumnsToFit();
	updateToolbar();
	emit layoutChange();
}

void GameListWidget::showGameGrid()
{
	Host::SetBaseBoolSettingValue("UI", "GameListGridView", true);
	Host::CommitBaseSettingChanges();

	if (m_ui.stack->currentIndex() == 1 || m_model->rowCount() == 0)
	{
		updateToolbar();
		return;
	}

	m_ui.stack->setCurrentIndex(1);
	setFocusProxy(m_ui.stack->currentWidget());
	updateToolbar();
	emit layoutChange();
}

void GameListWidget::setShowCoverTitles(bool enabled)
{
	if (m_model->getShowCoverTitles() == enabled)
		return;

	Host::SetBaseBoolSettingValue("UI", "GameListShowCoverTitles", enabled);
	Host::CommitBaseSettingChanges();
	m_model->setShowCoverTitles(enabled);
	if (isShowingGameGrid())
		m_model->refresh();
	updateToolbar();
	emit layoutChange();
}

void GameListWidget::setShowFullCoverTitles(bool enabled)
{
	if (m_model->getShowFullCoverTitles() == enabled)
		return;

	Host::SetBaseBoolSettingValue("UI", "GameListShowFullCoverTitles", enabled);
	Host::CommitBaseSettingChanges();
	m_model->setShowFullCoverTitles(enabled);
	m_list_view->setWordWrap(enabled); 
	if (isShowingGameGrid())
		m_model->refresh();
	updateToolbar();
	emit layoutChange();
}

void GameListWidget::updateToolbar()
{
	const bool grid_view = Host::GetBaseBoolSettingValue("UI", "GameListGridView", false);
	{
		QSignalBlocker sb(m_ui.viewGameGrid);
		m_ui.viewGameGrid->setChecked(grid_view);
	}
	{
		QSignalBlocker sb(m_ui.viewGameList);
		m_ui.viewGameList->setChecked(!grid_view);
	}
	{
		QSignalBlocker sb(m_ui.viewGridTitles);
		m_ui.viewGridTitles->setChecked(m_model->getShowCoverTitles());
	}
	{
		QSignalBlocker sb(m_ui.viewFullGridTitles);
		m_ui.viewFullGridTitles->setChecked(m_model->getShowFullCoverTitles());
	}
	{
		QSignalBlocker sb(m_ui.gridScale);
		m_ui.gridScale->setValue(static_cast<int>(m_model->getCoverScale() * 100.0f));
	}

	m_ui.viewGridTitles->setEnabled(grid_view);
	m_ui.viewFullGridTitles->setEnabled(grid_view && m_model->getShowCoverTitles());
	m_ui.gridScale->setEnabled(grid_view);
}

void GameListWidget::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	updateCustomBackgroundState();
	processBackgroundFrames();
}

void GameListWidget::hideEvent(QHideEvent* event)
{
	QWidget::hideEvent(event);
	updateCustomBackgroundState();
}

void GameListWidget::resizeEvent(QResizeEvent* event)
{
	QWidget::resizeEvent(event);
	resizeTableViewColumnsToFit();
	m_model->updateCacheSize(width(), height());
	processBackgroundFrames();
}

bool GameListWidget::event(QEvent* event)
{
	if (event->type() == QEvent::DevicePixelRatioChange)
	{
		m_model->setDevicePixelRatio(devicePixelRatioF());
		processBackgroundFrames();
		QWidget::event(event);
		return true;
	}

	return QWidget::event(event);
}

bool GameListWidget::eventFilter(QObject* watched, QEvent* event)
{
	if (watched == m_ui.stack && event->type() == QEvent::Paint)
	{
		if (!m_background_pixmap.isNull())
		{
			QPainter painter(m_ui.stack);
			painter.setClipRect(static_cast<QPaintEvent*>(event)->rect());
			painter.drawPixmap(0, 0, m_background_pixmap);
			return true;
		}
	}

	return QWidget::eventFilter(watched, event);
}

void GameListWidget::resizeTableViewColumnsToFit()
{
	QtUtils::ResizeColumnsForTableView(m_table_view, {
														 DEFAULT_COLUMN_WIDTHS[GameListModel::Column_Type],
														 DEFAULT_COLUMN_WIDTHS[GameListModel::Column_Serial],
														 DEFAULT_COLUMN_WIDTHS[GameListModel::Column_Title],
														 DEFAULT_COLUMN_WIDTHS[GameListModel::Column_FileTitle],
														 DEFAULT_COLUMN_WIDTHS[GameListModel::Column_CRC],
														 DEFAULT_COLUMN_WIDTHS[GameListModel::Column_TimePlayed],
														 DEFAULT_COLUMN_WIDTHS[GameListModel::Column_LastPlayed],
														 DEFAULT_COLUMN_WIDTHS[GameListModel::Column_Size],
														 DEFAULT_COLUMN_WIDTHS[GameListModel::Column_Region],
														 DEFAULT_COLUMN_WIDTHS[GameListModel::Column_Compatibility],
													 });
}

void GameListWidget::loadTableHeaderState()
{
	QHeaderView* header = m_table_view->horizontalHeader();
	if (!header)
		return;

	const std::string state_setting = Host::GetBaseStringSettingValue("GameListTableView", "HeaderState");
	if (state_setting.empty())
		return;

	QSignalBlocker blocker(header);
	header->restoreState(QByteArray::fromBase64(QByteArray::fromStdString(state_setting)));

	header->setSectionHidden(GameListModel::Column_Cover, true);

	ensureMinimumOneColumnVisible();
}

void GameListWidget::ensureMinimumOneColumnVisible()
{
	QHeaderView* header = m_table_view->horizontalHeader();
	if (!header)
		return;

	bool any_visible = false;
	for (int column = 0; column < GameListModel::Column_Count; column++)
	{
		if (column != GameListModel::Column_Cover && !header->isSectionHidden(column))
		{
			any_visible = true;
			break;
		}
	}

	if (!any_visible)
	{
		header->setSectionHidden(GameListModel::Column_Title, false);
		onTableHeaderStateChanged();
	}
}

void GameListWidget::onTableHeaderStateChanged()
{
	QHeaderView* header = m_table_view->horizontalHeader();
	if (!header)
		return;

	Host::SetBaseStringSettingValue("GameListTableView", "HeaderState", header->saveState().toBase64());
	Host::CommitBaseSettingChanges();
}

void GameListWidget::applyTableHeaderDefaults()
{
	QHeaderView* header = m_table_view->horizontalHeader();
	if (!header)
		return;

	{
		QSignalBlocker blocker(header);
		header->hideSection(GameListModel::Column_FileTitle);
		header->hideSection(GameListModel::Column_CRC);
		header->hideSection(GameListModel::Column_Cover);
		for (int column = 0; column < GameListModel::Column_Count; column++)
		{
			if (column == GameListModel::Column_Cover)
				continue;

			header->resizeSection(column, DEFAULT_COLUMN_WIDTHS[column]);
		}
		header->setSortIndicator(DEFAULT_SORT_INDEX, DEFAULT_SORT_ORDER);
	}

	Host::SetBaseStringSettingValue("GameListTableView", "HeaderState", header->saveState().toBase64());
	Host::CommitBaseSettingChanges();
}

void GameListWidget::resetTableHeaderToDefault()
{
	QHeaderView* header = m_table_view->horizontalHeader();
	if (!header)
		return;

	{
		QSignalBlocker blocker(header);
		for (int column = 0; column < GameListModel::Column_Count; column++)
		{
			if (column == GameListModel::Column_Cover)
				continue;

			header->resizeSection(column, DEFAULT_COLUMN_WIDTHS[column]);
			header->moveSection(header->visualIndex(column), column);
			header->setSectionHidden(column,
				column == GameListModel::Column_CRC || column == GameListModel::Column_FileTitle);
		}
		header->hideSection(GameListModel::Column_Cover);
		header->setSortIndicator(DEFAULT_SORT_INDEX, DEFAULT_SORT_ORDER);
	}

	Host::SetBaseStringSettingValue("GameListTableView", "HeaderState", header->saveState().toBase64());
	Host::CommitBaseSettingChanges();

	resizeTableViewColumnsToFit();
}

void GameListWidget::saveSortSettings(const int column, const Qt::SortOrder sort_order)
{
	Host::SetBaseStringSettingValue("GameListTableView", "SortColumn",
		GameListModel::getColumnName(static_cast<GameListModel::Column>(column)));
	Host::SetBaseBoolSettingValue("GameListTableView", "SortDescending", static_cast<bool>(sort_order));
	Host::CommitBaseSettingChanges();
}

std::optional<GameList::Entry> GameListWidget::getSelectedEntry() const
{
	auto lock = GameList::GetLock();

	const GameList::Entry* entry;
	if (m_ui.stack->currentIndex() == 0)
	{
		const QItemSelectionModel* selection_model = m_table_view->selectionModel();
		if (!selection_model->hasSelection())
			return std::nullopt;

		const QModelIndexList selected_rows = selection_model->selectedRows();
		if (selected_rows.empty())
			return std::nullopt;

		const QModelIndex source_index = m_sort_model->mapToSource(selected_rows[0]);
		if (!source_index.isValid())
			return std::nullopt;

		entry = GameList::GetEntryByIndex(source_index.row());
	}
	else
	{
		const QItemSelectionModel* selection_model = m_list_view->selectionModel();
		if (!selection_model->hasSelection())
			return std::nullopt;

		const QModelIndex source_index = m_sort_model->mapToSource(selection_model->currentIndex());
		if (!source_index.isValid())
			return std::nullopt;

		entry = GameList::GetEntryByIndex(source_index.row());
	}

	if (!entry)
		return std::nullopt;

	return *entry;
}

void GameListWidget::rescanFile(const std::string& path)
{
	if (QtHost::IsVMValid())
	{
		Console.Error(fmt::format("Can't re-scan ELF at '{}' because we have a VM running.", path));
		return;
	}

	GameList::RescanPath(path);
	m_model->refresh();
}

GameListGridListView::GameListGridListView(QWidget* parent )
	: QListView(parent)
{
}

void GameListGridListView::wheelEvent(QWheelEvent* e)
{
	if (e->modifiers() & Qt::ControlModifier)
	{
		int dy = e->angleDelta().y();
		if (dy != 0)
		{
			if (dy < 0)
				zoomOut();
			else
				zoomIn();

			return;
		}
	}

	QListView::wheelEvent(e);
}

#include "moc_GameListWidget.cpp"

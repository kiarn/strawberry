/*
 * Strawberry Music Player
 * Copyright 2018-2024, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <memory>
#include <functional>
#include <algorithm>
#include <utility>
#include <optional>

#include <QObject>
#include <QtGlobal>
#include <QtConcurrent>
#include <QThread>
#include <QMutex>
#include <QFuture>
#include <QFutureWatcher>
#include <QDataStream>
#include <QMimeData>
#include <QIODevice>
#include <QList>
#include <QSet>
#include <QMap>
#include <QMetaType>
#include <QVariant>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QImage>
#include <QChar>
#include <QRegularExpression>
#include <QPixmapCache>
#include <QNetworkDiskCache>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include "core/scoped_ptr.h"
#include "core/shared_ptr.h"
#include "core/application.h"
#include "core/database.h"
#include "core/iconloader.h"
#include "core/logging.h"
#include "core/taskmanager.h"
#include "core/sqlrow.h"
#include "collectionfilteroptions.h"
#include "collectionquery.h"
#include "collectionbackend.h"
#include "collectiondirectorymodel.h"
#include "collectionitem.h"
#include "collectionmodel.h"
#include "collectionmodelupdate.h"
#include "playlist/playlistmanager.h"
#include "playlist/songmimedata.h"
#include "covermanager/albumcoverloaderoptions.h"
#include "covermanager/albumcoverloaderresult.h"
#include "covermanager/albumcoverloader.h"
#include "settings/collectionsettingspage.h"

const int CollectionModel::kPrettyCoverSize = 32;
namespace {
constexpr char kPixmapDiskCacheDir[] = "pixmapcache";
constexpr char kVariousArtists[] = "Various artists";
}  // namespace

QNetworkDiskCache *CollectionModel::sIconCache = nullptr;

CollectionModel::CollectionModel(SharedPtr<CollectionBackend> backend, Application *app, QObject *parent)
    : SimpleTreeModel<CollectionItem>(new CollectionItem(this), parent),
      backend_(backend),
      app_(app),
      dir_model_(new CollectionDirectoryModel(backend, this)),
      show_various_artists_(true),
      sort_skips_articles_(true),
      total_song_count_(0),
      total_artist_count_(0),
      total_album_count_(0),
      separate_albums_by_grouping_(false),
      artist_icon_(IconLoader::Load("folder-sound")),
      album_icon_(IconLoader::Load("cdcase")),
      init_task_id_(-1),
      use_pretty_covers_(true),
      show_dividers_(true),
      use_disk_cache_(false),
      timer_reset_(new QTimer(this)),
      timer_update_(new QTimer(this)) {

  group_by_[0] = GroupBy::AlbumArtist;
  group_by_[1] = GroupBy::AlbumDisc;
  group_by_[2] = GroupBy::None;

  if (app_) {
    QObject::connect(&*app_->album_cover_loader(), &AlbumCoverLoader::AlbumCoverLoaded, this, &CollectionModel::AlbumCoverLoaded);
  }

  QIcon nocover = IconLoader::Load("cdcase");
  if (!nocover.isNull()) {
    QList<QSize> nocover_sizes = nocover.availableSizes();
    no_cover_icon_ = nocover.pixmap(nocover_sizes.last()).scaled(kPrettyCoverSize, kPrettyCoverSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  }

  if (app_ && !sIconCache) {
    sIconCache = new QNetworkDiskCache(this);
    sIconCache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/" + kPixmapDiskCacheDir);
    QObject::connect(app_, &Application::ClearPixmapDiskCache, this, &CollectionModel::ClearDiskCache);
  }

  QObject::connect(&*backend_, &CollectionBackend::SongsAdded, this, &CollectionModel::SongsAdded);
  QObject::connect(&*backend_, &CollectionBackend::SongsDeleted, this, &CollectionModel::SongsRemoved);
  QObject::connect(&*backend_, &CollectionBackend::SongsChanged, this, &CollectionModel::SongsChanged);
  QObject::connect(&*backend_, &CollectionBackend::DatabaseReset, this, &CollectionModel::ScheduleReset);
  QObject::connect(&*backend_, &CollectionBackend::TotalSongCountUpdated, this, &CollectionModel::TotalSongCountUpdatedSlot);
  QObject::connect(&*backend_, &CollectionBackend::TotalArtistCountUpdated, this, &CollectionModel::TotalArtistCountUpdatedSlot);
  QObject::connect(&*backend_, &CollectionBackend::TotalAlbumCountUpdated, this, &CollectionModel::TotalAlbumCountUpdatedSlot);
  QObject::connect(&*backend_, &CollectionBackend::SongsStatisticsChanged, this, &CollectionModel::SongsChanged);
  QObject::connect(&*backend_, &CollectionBackend::SongsRatingChanged, this, &CollectionModel::SongsChanged);

  backend_->UpdateTotalSongCountAsync();
  backend_->UpdateTotalArtistCountAsync();
  backend_->UpdateTotalAlbumCountAsync();

  timer_reset_->setSingleShot(true);
  timer_reset_->setInterval(300);
  QObject::connect(timer_reset_, &QTimer::timeout, this, &CollectionModel::Reload);

  timer_update_->setSingleShot(false);
  timer_update_->setInterval(20);
  QObject::connect(timer_update_, &QTimer::timeout, this, &CollectionModel::ProcessUpdate);

  ReloadSettings();

}

CollectionModel::~CollectionModel() {

  qLog(Debug) << "Collection model" << this << "for" << Song::TextForSource(backend_->source()) << "deleted";

  beginResetModel();
  Clear();
  endResetModel();

}

void CollectionModel::Init() {
  ScheduleReset();
}

void CollectionModel::Reset() {
  ScheduleReset();
}

void CollectionModel::Clear() {

  if (root_) {
    delete root_;
    root_ = nullptr;
  }
  song_nodes_.clear();
  container_nodes_[0].clear();
  container_nodes_[1].clear();
  container_nodes_[2].clear();
  divider_nodes_.clear();
  pending_art_.clear();
  pending_cache_keys_.clear();

}

void CollectionModel::BeginReset() {

  beginResetModel();
  Clear();
  root_ = new CollectionItem(this);

}

void CollectionModel::EndReset() {

  endResetModel();

}

void CollectionModel::Reload() {

  BeginReset();

  // Show a loading indicator in the model.
  CollectionItem *loading = new CollectionItem(CollectionItem::Type_LoadingIndicator, root_);
  loading->display_text = tr("Loading...");

  // Show a loading indicator in the status bar too.
  if (app_) {
    init_task_id_ = app_->task_manager()->StartTask(tr("Loading songs"));
  }

  EndReset();

  StartLoadSongsFromSql();

}

void CollectionModel::ScheduleReset() {

  if (!timer_reset_->isActive()) {
    timer_reset_->start();
  }

}

void CollectionModel::ReloadSettings() {

  QSettings s;

  s.beginGroup(CollectionSettingsPage::kSettingsGroup);

  use_disk_cache_ = s.value(CollectionSettingsPage::kSettingsDiskCacheEnable, false).toBool();

  QPixmapCache::setCacheLimit(static_cast<int>(MaximumCacheSize(&s, CollectionSettingsPage::kSettingsCacheSize, CollectionSettingsPage::kSettingsCacheSizeUnit, CollectionSettingsPage::kSettingsCacheSizeDefault) / 1024));

  if (sIconCache) {
    sIconCache->setMaximumCacheSize(MaximumCacheSize(&s, CollectionSettingsPage::kSettingsDiskCacheSize, CollectionSettingsPage::kSettingsDiskCacheSizeUnit, CollectionSettingsPage::kSettingsDiskCacheSizeDefault));
  }

  s.endGroup();

  cover_types_ = AlbumCoverLoaderOptions::LoadTypes();

  if (!use_disk_cache_) {
    ClearDiskCache();
  }

}

void CollectionModel::set_pretty_covers(const bool use_pretty_covers) {

  if (use_pretty_covers != use_pretty_covers_) {
    use_pretty_covers_ = use_pretty_covers;
    ScheduleReset();
  }

}

void CollectionModel::set_show_dividers(const bool show_dividers) {

  if (show_dividers != show_dividers_) {
    show_dividers_ = show_dividers;
    ScheduleReset();
  }

}

void CollectionModel::set_sort_skips_articles(const bool sort_skips_articles) {

  if (sort_skips_articles != sort_skips_articles_) {
    sort_skips_articles_ = sort_skips_articles;
    ScheduleReset();
  }

}

void CollectionModel::SongsAdded(const SongList &songs) {

  ScheduleUpdate(CollectionModelUpdate::Type::Add, songs);

}

void CollectionModel::SongsRemoved(const SongList &songs) {

  ScheduleUpdate(CollectionModelUpdate::Type::Remove, songs);

}

void CollectionModel::SongsChanged(const SongList &songs) {

  ScheduleUpdate(CollectionModelUpdate::Type::ReAddOrUpdate, songs);

}

void CollectionModel::SongsUpdated(const SongList &songs) {

  ScheduleUpdate(CollectionModelUpdate::Type::Update, songs);

}

void CollectionModel::ScheduleUpdate(const CollectionModelUpdate::Type type, const SongList &songs) {

  for (qsizetype i = 0; i < songs.count(); i += 400LL) {
    const auto number = std::min(songs.count() - i, 400LL);
    const SongList songs_to_queue = songs.mid(i, number);
    updates_.enqueue(CollectionModelUpdate(type, songs_to_queue));
  }

  if (!timer_update_->isActive()) {
    timer_update_->start();
  }

}

void CollectionModel::ProcessUpdate() {

  if (updates_.isEmpty()) {
    timer_update_->stop();
    return;
  }

  const CollectionModelUpdate update = updates_.dequeue();

  if (updates_.isEmpty()) {
    timer_update_->stop();
  }

  switch (update.type) {
    case CollectionModelUpdate::Type::Add:
      AddSongs(update.songs);
      break;
    case CollectionModelUpdate::Type::Remove:
      RemoveSongs(update.songs);
      break;
    case CollectionModelUpdate::Type::ReAddOrUpdate:
      ReAddOrUpdate(update.songs);
      break;
    case CollectionModelUpdate::Type::Update:
      UpdateSongs(update.songs);
      break;
  }

}

void CollectionModel::AddSongs(const SongList &songs) {

  for (const Song &song : songs) {

    if (songs_.contains(song.id())) {
      songs_[song.id()] = song;
    }
    else {
      songs_.insert(song.id(), song);
    }

    // Sanity check to make sure we don't add songs that are outside the user's filter
    if (!filter_options_.Matches(song)) continue;

    // Hey, we've already got that one!
    if (song_nodes_.contains(song.id())) continue;

    // Before we can add each song we need to make sure the required container items already exist in the tree.
    // These depend on which "group by" settings the user has on the collection.
    // Eg. if the user grouped by artist and album, we would need to make sure nodes for the song's artist and album were already in the tree.

    // Find parent containers in the tree
    CollectionItem *container = root_;
    QString key;
    for (int i = 0; i < 3; ++i) {
      GroupBy group_by = group_by_[i];
      if (group_by == GroupBy::None) break;

      if (!key.isEmpty()) key.append("-");

      // Special case: if the song is a compilation and the current GroupBy level is Artists, then we want the Various Artists node :(
      if (IsArtistGroupBy(group_by) && song.is_compilation()) {
        if (container->compilation_artist_node_ == nullptr) {
          CreateCompilationArtistNode(true, container);
        }
        container = container->compilation_artist_node_;
        key = container->key;
      }
      else {
        // Otherwise find the proper container at this level based on the item's key
        key.append(ContainerKey(group_by, separate_albums_by_grouping_, song));

        // Does it exist already?
        if (container_nodes_[i].contains(key)) {
          container = container_nodes_[i][key];
        }
        else {
          // Create the container
          container = ItemFromSong(group_by, separate_albums_by_grouping_, true, i == 0, container, song, i);
          container_nodes_[i].insert(key, container);
        }

      }

    }

    song_nodes_.insert(song.id(), ItemFromSong(GroupBy::None, separate_albums_by_grouping_, true, false, container, song, -1));
  }

}

void CollectionModel::ReAddOrUpdate(const SongList &songs) {

  SongList songs_added;
  SongList songs_removed;
  SongList songs_updated;

  for (const Song &song : songs) {
    if (!song_nodes_.contains(song.id())) {
      qLog(Error) << "Song does not exist in model" << song.effective_albumartist() << song.effective_album() << song.title();
      continue;
    }
    const Song &metadata = song_nodes_[song.id()]->metadata;
    bool container_key_changed = false;
    for (int i = 0; i < 3; ++i) {
      if (ContainerKey(group_by_[i], separate_albums_by_grouping_, song) != ContainerKey(group_by_[i], separate_albums_by_grouping_, metadata)) {
        container_key_changed = true;
      }
    }
    if (container_key_changed) {
      songs_removed << metadata;
      songs_added << song;
    }
    else {
      songs_updated << song;
    }
  }

  SongsUpdated(songs_updated);
  SongsRemoved(songs_removed);
  SongsAdded(songs_added);

}

void CollectionModel::UpdateSongs(const SongList &songs) {

  for (const Song &song : songs) {
    if (songs_.contains(song.id())) {
      songs_[song.id()] = song;
    }
    if (!song_nodes_.contains(song.id())) {
      qLog(Error) << "Song does not exist in model" << song.effective_albumartist() << song.effective_album() << song.title();
      continue;
    }
    CollectionItem *item = song_nodes_[song.id()];
    const Song &metdata = item->metadata;
    const bool data_changed = !IsCollectionMetadataEqual(song, metdata);
    item->metadata = song;
    if (data_changed) {
      const QModelIndex idx = ItemToIndex(item);
      if (!idx.isValid()) return;
      emit dataChanged(idx, idx);
    }
  }

}

CollectionItem *CollectionModel::CreateCompilationArtistNode(const bool signal, CollectionItem *parent) {

  Q_ASSERT(parent->compilation_artist_node_ == nullptr);

  if (signal) beginInsertRows(ItemToIndex(parent), static_cast<int>(parent->children.count()), static_cast<int>(parent->children.count()));

  parent->compilation_artist_node_ = new CollectionItem(CollectionItem::Type_Container, parent);
  parent->compilation_artist_node_->compilation_artist_node_ = nullptr;
  if (parent != root_ && !parent->key.isEmpty()) parent->compilation_artist_node_->key.append(parent->key);
  parent->compilation_artist_node_->key.append(tr(kVariousArtists));
  parent->compilation_artist_node_->display_text = tr(kVariousArtists);
  parent->compilation_artist_node_->sort_text = " various";
  parent->compilation_artist_node_->container_level = parent->container_level + 1;

  if (signal) endInsertRows();

  return parent->compilation_artist_node_;

}

QString CollectionModel::ContainerKey(const GroupBy group_by, const bool separate_albums_by_grouping, const Song &song) {

  QString key;

  switch (group_by) {
    case GroupBy::AlbumArtist:
      key = TextOrUnknown(song.effective_albumartist());
      break;
    case GroupBy::Artist:
      key = TextOrUnknown(song.artist());
      break;
    case GroupBy::Album:
      key = TextOrUnknown(song.album());
      if (!song.album_id().isEmpty()) key.append("-" + song.album_id());
      if (separate_albums_by_grouping && !song.grouping().isEmpty()) key.append("-" + song.grouping());
      break;
    case GroupBy::AlbumDisc:
      key = PrettyAlbumDisc(song.album(), song.disc());
      if (!song.album_id().isEmpty()) key.append("-" + song.album_id());
      if (separate_albums_by_grouping && !song.grouping().isEmpty()) key.append("-" + song.grouping());
      break;
    case GroupBy::YearAlbum:
      key = PrettyYearAlbum(song.year(), song.album());
      if (!song.album_id().isEmpty()) key.append("-" + song.album_id());
      if (separate_albums_by_grouping && !song.grouping().isEmpty()) key.append("-" + song.grouping());
      break;
    case GroupBy::YearAlbumDisc:
      key = PrettyYearAlbumDisc(song.year(), song.album(), song.disc());
      if (!song.album_id().isEmpty()) key.append("-" + song.album_id());
      if (separate_albums_by_grouping && !song.grouping().isEmpty()) key.append("-" + song.grouping());
      break;
    case GroupBy::OriginalYearAlbum:
      key = PrettyYearAlbum(song.effective_originalyear(), song.album());
      if (!song.album_id().isEmpty()) key.append("-" + song.album_id());
      if (separate_albums_by_grouping && !song.grouping().isEmpty()) key.append("-" + song.grouping());
      break;
    case GroupBy::OriginalYearAlbumDisc:
      key = PrettyYearAlbumDisc(song.effective_originalyear(), song.album(), song.disc());
      if (!song.album_id().isEmpty()) key.append("-" + song.album_id());
      if (separate_albums_by_grouping && !song.grouping().isEmpty()) key.append("-" + song.grouping());
      break;
    case GroupBy::Disc:
      key = PrettyDisc(song.disc());
      break;
    case GroupBy::Year:
      key = QString::number(std::max(0, song.year()));
      break;
    case GroupBy::OriginalYear:
      key = QString::number(std::max(0, song.effective_originalyear()));
      break;
    case GroupBy::Genre:
      key = TextOrUnknown(song.genre());
      break;
    case GroupBy::Composer:
      key = TextOrUnknown(song.composer());
      break;
    case GroupBy::Performer:
      key = TextOrUnknown(song.performer());
      break;
    case GroupBy::Grouping:
      key = TextOrUnknown(song.grouping());
      break;
    case GroupBy::FileType:
      key = song.TextForFiletype();
      break;
    case GroupBy::Samplerate:
      key = QString::number(std::max(0, song.samplerate()));
      break;
    case GroupBy::Bitdepth:
      key = QString::number(std::max(0, song.bitdepth()));
      break;
    case GroupBy::Bitrate:
      key = QString::number(std::max(0, song.bitrate()));
      break;
    case GroupBy::Format:
      if (song.samplerate() <= 0) {
        key = song.TextForFiletype();
      }
      else {
        if (song.bitdepth() <= 0) {
          key = QString("%1 (%2)").arg(song.TextForFiletype(), QString::number(song.samplerate() / 1000.0, 'G', 5));
        }
        else {
          key = QString("%1 (%2/%3)").arg(song.TextForFiletype(), QString::number(song.samplerate() / 1000.0, 'G', 5)).arg(song.bitdepth());
        }
      }
      break;
    case GroupBy::None:
    case GroupBy::GroupByCount:
      qLog(Error) << "GroupBy::None";
      break;
  }

  return key;

}

QString CollectionModel::DividerKey(const GroupBy group_by, CollectionItem *item) {

  // Items which are to be grouped under the same divider must produce the same divider key.  This will only get called for top-level items.

  if (item->sort_text.isEmpty()) return QString();

  switch (group_by) {
    case GroupBy::AlbumArtist:
    case GroupBy::Artist:
    case GroupBy::Album:
    case GroupBy::AlbumDisc:
    case GroupBy::Composer:
    case GroupBy::Performer:
    case GroupBy::Grouping:
    case GroupBy::Disc:
    case GroupBy::Genre:
    case GroupBy::Format:
    case GroupBy::FileType: {
      QChar c = item->sort_text[0];
      if (c.isDigit()) return "0";
      if (c == ' ') return QString();
      if (c.decompositionTag() != QChar::NoDecomposition) {
        QString decomposition = c.decomposition();
        return QChar(decomposition[0]);
      }
      return c;
    }

    case GroupBy::Year:
    case GroupBy::OriginalYear:
      return SortTextForNumber(item->sort_text.toInt() / 10 * 10);

    case GroupBy::YearAlbum:
    case GroupBy::YearAlbumDisc:
      return SortTextForNumber(item->metadata.year());

    case GroupBy::OriginalYearAlbum:
    case GroupBy::OriginalYearAlbumDisc:
      return SortTextForNumber(item->metadata.effective_originalyear());

    case GroupBy::Samplerate:
      return SortTextForNumber(item->metadata.samplerate());

    case GroupBy::Bitdepth:
      return SortTextForNumber(item->metadata.bitdepth());

    case GroupBy::Bitrate:
      return SortTextForNumber(item->metadata.bitrate());

    case GroupBy::None:
    case GroupBy::GroupByCount:
      return QString();
  }
  qLog(Error) << "Unknown GroupBy" << group_by << "for item" << item->display_text;
  return QString();

}

QString CollectionModel::DividerDisplayText(const GroupBy group_by, const QString &key) {

  // Pretty display text for the dividers.

  switch (group_by) {
    case GroupBy::AlbumArtist:
    case GroupBy::Artist:
    case GroupBy::Album:
    case GroupBy::AlbumDisc:
    case GroupBy::Composer:
    case GroupBy::Performer:
    case GroupBy::Disc:
    case GroupBy::Grouping:
    case GroupBy::Genre:
    case GroupBy::FileType:
    case GroupBy::Format:
      if (key == "0") return "0-9";
      return key.toUpper();

    case GroupBy::YearAlbum:
    case GroupBy::YearAlbumDisc:
    case GroupBy::OriginalYearAlbum:
    case GroupBy::OriginalYearAlbumDisc:
      if (key == "0000") return tr("Unknown");
      return key.toUpper();

    case GroupBy::Year:
    case GroupBy::OriginalYear:
      if (key == "0000") return tr("Unknown");
      return QString::number(key.toInt());  // To remove leading 0s

    case GroupBy::Samplerate:
      if (key == "000") return tr("Unknown");
      return QString::number(key.toInt());  // To remove leading 0s

    case GroupBy::Bitdepth:
      if (key == "000") return tr("Unknown");
      return QString::number(key.toInt());  // To remove leading 0s

    case GroupBy::Bitrate:
      if (key == "000") return tr("Unknown");
      return QString::number(key.toInt());  // To remove leading 0s

    case GroupBy::None:
    case GroupBy::GroupByCount:
      break;
  }
  qLog(Error) << "Unknown GroupBy" << group_by << "for divider key" << key;
  return QString();

}

void CollectionModel::RemoveSongs(const SongList &songs) {

  // Delete the actual song nodes first, keeping track of each parent so we might check to see if they're empty later.
  QSet<CollectionItem*> parents;
  for (const Song &song : songs) {

    if (songs_.contains(song.id())) {
      songs_[song.id()] = song;
    }

    if (song_nodes_.contains(song.id())) {
      CollectionItem *node = song_nodes_[song.id()];

      if (node->parent != root_) parents << node->parent;

      beginRemoveRows(ItemToIndex(node->parent), node->row, node->row);
      node->parent->Delete(node->row);
      song_nodes_.remove(song.id());
      endRemoveRows();

    }
  }

  // Now delete empty parents
  QSet<QString> divider_keys;
  while (!parents.isEmpty()) {
    // Since we are going to remove elements from the container, we need a copy to iterate over.
    // If we iterate over the original, the behavior will be undefined.
    QSet<CollectionItem*> parents_copy = parents;
    for (CollectionItem *node : parents_copy) {
      parents.remove(node);
      if (node->children.count() != 0) continue;

      // Consider its parent for the next round
      if (node->parent != root_) parents << node->parent;

      // Maybe consider its divider node
      if (node->container_level == 0) {
        divider_keys << DividerKey(group_by_[0], node);
      }

      // Special case the Various Artists node
      if (IsCompilationArtistNode(node)) {
        node->parent->compilation_artist_node_ = nullptr;
      }
      else if (container_nodes_[node->container_level].contains(node->key)) {
        container_nodes_[node->container_level].remove(node->key);
      }

      // Remove from pixmap cache
      const QString cache_key = AlbumIconPixmapCacheKey(ItemToIndex(node));
      QPixmapCache::remove(cache_key);
      if (use_disk_cache_ && sIconCache) sIconCache->remove(AlbumIconPixmapDiskCacheKey(cache_key));
      if (pending_cache_keys_.contains(cache_key)) {
        pending_cache_keys_.remove(cache_key);
      }

      // Remove from pending art loading
      for (QMap<quint64, ItemAndCacheKey>::iterator it = pending_art_.begin(); it != pending_art_.end();) {
        if (it.value().first == node) {
          it = pending_art_.erase(it);
        }
        else {
          ++it;
        }
      }

      // It was empty - delete it
      beginRemoveRows(ItemToIndex(node->parent), node->row, node->row);
      node->parent->Delete(node->row);
      endRemoveRows();
    }
  }

  // Delete empty dividers
  for (const QString &divider_key : std::as_const(divider_keys)) {
    if (!divider_nodes_.contains(divider_key)) continue;

    // Look to see if there are any other items still under this divider
    QList<CollectionItem*> container_nodes = container_nodes_[0].values();
    if (std::any_of(container_nodes.begin(), container_nodes.end(), [this, divider_key](CollectionItem *node){ return DividerKey(group_by_[0], node) == divider_key; })) {
      continue;
    }

    // Remove the divider
    int row = divider_nodes_[divider_key]->row;
    beginRemoveRows(ItemToIndex(root_), row, row);
    root_->Delete(row);
    endRemoveRows();
    divider_nodes_.remove(divider_key);
  }

}

QString CollectionModel::AlbumIconPixmapCacheKey(const QModelIndex &idx) const {

  QStringList path;
  QModelIndex idx_copy(idx);
  while (idx_copy.isValid()) {
    path.prepend(idx_copy.data().toString());
    idx_copy = idx_copy.parent();
  }

  return Song::TextForSource(backend_->source()) + "/" + path.join("/");

}

QUrl CollectionModel::AlbumIconPixmapDiskCacheKey(const QString &cache_key) const {

  return QUrl(QUrl::toPercentEncoding(cache_key));

}

QVariant CollectionModel::AlbumIcon(const QModelIndex &idx) {

  CollectionItem *item = IndexToItem(idx);
  if (!item) return no_cover_icon_;

  // Check the cache for a pixmap we already loaded.
  const QString cache_key = AlbumIconPixmapCacheKey(idx);

  QPixmap cached_pixmap;
  if (QPixmapCache::find(cache_key, &cached_pixmap)) {
    return cached_pixmap;
  }

  // Try to load it from the disk cache
  if (use_disk_cache_ && sIconCache) {
    ScopedPtr<QIODevice> disk_cache_img(sIconCache->data(AlbumIconPixmapDiskCacheKey(cache_key)));
    if (disk_cache_img) {
      QImage cached_image;
      if (cached_image.load(&*disk_cache_img, "XPM")) {
        QPixmapCache::insert(cache_key, QPixmap::fromImage(cached_image));
        return QPixmap::fromImage(cached_image);
      }
    }
  }

  // Maybe we're loading a pixmap already?
  if (pending_cache_keys_.contains(cache_key)) {
    return no_cover_icon_;
  }

  // No art is cached and we're not loading it already.  Load art for the first song in the album.
  SongList songs = GetChildSongs(idx);
  if (!songs.isEmpty()) {
    AlbumCoverLoaderOptions cover_loader_options(AlbumCoverLoaderOptions::Option::ScaledImage | AlbumCoverLoaderOptions::Option::PadScaledImage);
    cover_loader_options.desired_scaled_size = QSize(kPrettyCoverSize, kPrettyCoverSize);
    cover_loader_options.types = cover_types_;
    const quint64 id = app_->album_cover_loader()->LoadImageAsync(cover_loader_options, songs.first());
    pending_art_[id] = ItemAndCacheKey(item, cache_key);
    pending_cache_keys_.insert(cache_key);
  }

  return no_cover_icon_;

}

void CollectionModel::AlbumCoverLoaded(const quint64 id, const AlbumCoverLoaderResult &result) {

  if (!pending_art_.contains(id)) return;

  ItemAndCacheKey item_and_cache_key = pending_art_.take(id);
  CollectionItem *item = item_and_cache_key.first;
  if (!item) return;

  const QString &cache_key = item_and_cache_key.second;

  pending_cache_keys_.remove(cache_key);

  // Insert this image in the cache.
  if (!result.success || result.image_scaled.isNull() || result.type == AlbumCoverLoaderResult::Type::Unset) {
    // Set the no_cover image so we don't continually try to load art.
    QPixmapCache::insert(cache_key, no_cover_icon_);
  }
  else {
    QPixmap image_pixmap;
    image_pixmap = QPixmap::fromImage(result.image_scaled);
    QPixmapCache::insert(cache_key, image_pixmap);
  }

  // If we have a valid cover not already in the disk cache
  if (use_disk_cache_ && sIconCache && result.success && !result.image_scaled.isNull()) {
    const QUrl disk_cache_key = AlbumIconPixmapDiskCacheKey(cache_key);
    ScopedPtr<QIODevice> disk_cache_img(sIconCache->data(disk_cache_key));
    if (!disk_cache_img) {
      QNetworkCacheMetaData disk_cache_metadata;
      disk_cache_metadata.setSaveToDisk(true);
      disk_cache_metadata.setUrl(disk_cache_key);
      // Qt 6 now ignores any entry without headers, so add a fake header.
      disk_cache_metadata.setRawHeaders(QNetworkCacheMetaData::RawHeaderList() << qMakePair(QByteArray(), QByteArray()));
      QIODevice *device_iconcache = sIconCache->prepare(disk_cache_metadata);
      if (device_iconcache) {
        result.image_scaled.save(device_iconcache, "XPM");
        sIconCache->insert(device_iconcache);
      }
    }
  }

  const QModelIndex idx = ItemToIndex(item);
  if (!idx.isValid()) return;

  emit dataChanged(idx, idx);

}

QVariant CollectionModel::data(const QModelIndex &idx, const int role) const {

  const CollectionItem *item = IndexToItem(idx);

  // Handle a special case for returning album artwork instead of a generic CD icon.
  // this is here instead of in the other data() function to let us use the
  // QModelIndex& version of GetChildSongs, which satisfies const-ness, instead
  // of the CollectionItem *version, which doesn't.
  if (use_pretty_covers_) {
    bool is_album_node = false;
    if (role == Qt::DecorationRole && item->type == CollectionItem::Type_Container) {
      GroupBy container_group_by = group_by_[item->container_level];
      is_album_node = IsAlbumGroupBy(container_group_by);
    }
    if (is_album_node) {
      // It has const behaviour some of the time - that's ok right?
      return const_cast<CollectionModel*>(this)->AlbumIcon(idx);
    }
  }

  return data(item, role);

}

QVariant CollectionModel::data(const CollectionItem *item, const int role) const {

  GroupBy container_group_by = item->type == CollectionItem::Type_Container ? group_by_[item->container_level] : GroupBy::None;

  switch (role) {
    case Qt::DisplayRole:
    case Qt::ToolTipRole:
      return item->DisplayText();

    case Qt::DecorationRole:
      switch (item->type) {
        case CollectionItem::Type_Container:
          switch (container_group_by) {
            case GroupBy::Album:
            case GroupBy::AlbumDisc:
            case GroupBy::YearAlbum:
            case GroupBy::YearAlbumDisc:
            case GroupBy::OriginalYearAlbum:
            case GroupBy::OriginalYearAlbumDisc:
              return album_icon_;
            case GroupBy::Artist:
            case GroupBy::AlbumArtist:
              return artist_icon_;
            default:
              break;
          }
          break;
        default:
          break;
      }
      break;

    case Role_Type:
      return item->type;

    case Role_IsDivider:
      return item->type == CollectionItem::Type_Divider;

    case Role_ContainerType:
      return static_cast<int>(container_group_by);

    case Role_Key:
      return item->key;

    case Role_Artist:
      return item->metadata.artist();

    case Role_Editable:{
      if (item->type == CollectionItem::Type_Container) {
        // If we have even one non editable item as a child, we ourselves are not available for edit
        if (item->children.isEmpty()) {
          return false;
        }
        else if (std::any_of(item->children.begin(), item->children.end(), [this, role](CollectionItem *child) { return !data(child, role).toBool(); })) {
          return false;
        }
        else {
          return true;
        }
      }
      else if (item->type == CollectionItem::Type_Song) {
        return item->metadata.IsEditable();
      }
      else {
        return false;
      }
    }

    case Role_SortText:
      return item->SortText();
    default:
      return QVariant();
  }

  return QVariant();

}

void CollectionModel::StartLoadSongsFromSql() {

  songs_.clear();

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  QFuture<SongList> future = QtConcurrent::run(&CollectionModel::LoadSongsFromSql, this, filter_options_);
#else
  QFuture<SongList> future = QtConcurrent::run(this, &CollectionModel::LoadSongsFromSql, filter_options_);
#endif
  QFutureWatcher<SongList> *watcher = new QFutureWatcher<SongList>();
  QObject::connect(watcher, &QFutureWatcher<CollectionModel::QueryResult>::finished, this, &CollectionModel::LoadSongsFromSqlFinished);
  watcher->setFuture(future);

}

SongList CollectionModel::LoadSongsFromSql(const CollectionFilterOptions &filter_options) {

  SongList songs;

  {
    QMutexLocker l(backend_->db()->Mutex());
    QSqlDatabase db(backend_->db()->Connect());
    CollectionQuery q(db, backend_->songs_table(), filter_options);
    q.SetColumnSpec("%songs_table.ROWID, " + Song::kColumnSpec);
    if (q.Exec()) {
      while (q.Next()) {
        Song song;
        song.InitFromQuery(q, true);
        songs << song;
      }
    }
    else {
      backend_->ReportErrors(q);
    }
  }

  if (QThread::currentThread() != thread() && QThread::currentThread() != backend_->thread()) {
    backend_->db()->Close();
  }

  return songs;

}

void CollectionModel::LoadSongsFromSqlFinished() {

  QFutureWatcher<SongList> *watcher = static_cast<QFutureWatcher<SongList>*>(sender());
  const SongList songs = watcher->result();
  watcher->deleteLater();

  BeginReset();
  SongsAdded(songs);
  EndReset();

  if (init_task_id_ != -1) {
    if (app_) {
      app_->task_manager()->SetTaskFinished(init_task_id_);
    }
    init_task_id_ = -1;
  }


}

CollectionItem *CollectionModel::InitItem(const GroupBy group_by, const bool signal, CollectionItem *parent, const int container_level) {

  CollectionItem::Type item_type = group_by == GroupBy::None ? CollectionItem::Type_Song : CollectionItem::Type_Container;

  if (signal) beginInsertRows(ItemToIndex(parent), static_cast<int>(parent->children.count()), static_cast<int>(parent->children.count()));

  // Initialize the item depending on what type it's meant to be
  CollectionItem *item = new CollectionItem(item_type, parent);
  item->compilation_artist_node_ = nullptr;
  item->container_level = container_level;

  return item;

}

CollectionItem *CollectionModel::ItemFromSong(const GroupBy group_by, const bool separate_albums_by_grouping, const bool signal, const bool create_divider, CollectionItem *parent, const Song &s, const int container_level) {

  CollectionItem *item = InitItem(group_by, signal, parent, container_level);

  if (parent != root_ && !parent->key.isEmpty()) {
    item->key = parent->key + "-";
  }

  switch (group_by) {
    case GroupBy::AlbumArtist:{
      item->metadata.set_albumartist(s.effective_albumartist());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      item->display_text = TextOrUnknown(s.effective_albumartist());
      item->sort_text = SortTextForArtist(s.effective_albumartist(), sort_skips_articles_);
      break;
    }
    case GroupBy::Artist:{
      item->metadata.set_artist(s.artist());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      item->display_text = TextOrUnknown(s.artist());
      item->sort_text = SortTextForArtist(s.artist(), sort_skips_articles_);
      break;
    }
    case GroupBy::Album:{
      item->metadata.set_album(s.album());
      item->metadata.set_album_id(s.album_id());
      item->metadata.set_grouping(s.grouping());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      item->display_text = TextOrUnknown(s.album());
      item->sort_text = SortTextForArtist(s.album(), sort_skips_articles_);
      break;
    }
    case GroupBy::AlbumDisc:{
      item->metadata.set_album(s.album());
      item->metadata.set_album_id(s.album_id());
      item->metadata.set_disc(s.disc() <= 0 ? -1 : s.disc());
      item->metadata.set_grouping(s.grouping());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      item->display_text = PrettyAlbumDisc(s.album(), s.disc());
      item->sort_text = s.album() + SortTextForNumber(std::max(0, s.disc()));
      break;
    }
    case GroupBy::YearAlbum:{
      item->metadata.set_year(s.year() <= 0 ? -1 : s.year());
      item->metadata.set_album(s.album());
      item->metadata.set_album_id(s.album_id());
      item->metadata.set_grouping(s.grouping());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      item->display_text = PrettyYearAlbum(s.year(), s.album());
      item->sort_text = SortTextForNumber(std::max(0, s.year())) + s.grouping() + s.album();
      break;
    }
    case GroupBy::YearAlbumDisc:{
      item->metadata.set_year(s.year() <= 0 ? -1 : s.year());
      item->metadata.set_album(s.album());
      item->metadata.set_album_id(s.album_id());
      item->metadata.set_disc(s.disc() <= 0 ? -1 : s.disc());
      item->metadata.set_grouping(s.grouping());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      item->display_text = PrettyYearAlbumDisc(s.year(), s.album(), s.disc());
      item->sort_text = SortTextForNumber(std::max(0, s.year())) + s.album() + SortTextForNumber(std::max(0, s.disc()));
      break;
    }
    case GroupBy::OriginalYearAlbum:{
      item->metadata.set_year(s.year() <= 0 ? -1 : s.year());
      item->metadata.set_originalyear(s.originalyear() <= 0 ? -1 : s.originalyear());
      item->metadata.set_album(s.album());
      item->metadata.set_album_id(s.album_id());
      item->metadata.set_grouping(s.grouping());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      item->display_text = PrettyYearAlbum(s.effective_originalyear(), s.album());
      item->sort_text = SortTextForNumber(std::max(0, s.effective_originalyear())) + s.grouping() + s.album();
      break;
    }
    case GroupBy::OriginalYearAlbumDisc:{
      item->metadata.set_year(s.year() <= 0 ? -1 : s.year());
      item->metadata.set_originalyear(s.originalyear() <= 0 ? -1 : s.originalyear());
      item->metadata.set_album(s.album());
      item->metadata.set_album_id(s.album_id());
      item->metadata.set_disc(s.disc() <= 0 ? -1 : s.disc());
      item->metadata.set_grouping(s.grouping());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      item->display_text = PrettyYearAlbumDisc(s.effective_originalyear(), s.album(), s.disc());
      item->sort_text = SortTextForNumber(std::max(0, s.effective_originalyear())) + s.album() + SortTextForNumber(std::max(0, s.disc()));
      break;
    }
    case GroupBy::Disc:{
      item->metadata.set_disc(s.disc() <= 0 ? -1 : s.disc());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      const int disc = std::max(0, s.disc());
      item->display_text = PrettyDisc(disc);
      item->sort_text = SortTextForNumber(disc);
      break;
    }
    case GroupBy::Year:{
      item->metadata.set_year(s.year() <= 0 ? -1 : s.year());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      const int year = std::max(0, s.year());
      item->display_text = QString::number(year);
      item->sort_text = SortTextForNumber(year) + " ";
      break;
    }
    case GroupBy::OriginalYear:{
      item->metadata.set_originalyear(s.effective_originalyear() <= 0 ? -1 : s.effective_originalyear());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      const int year = std::max(0, s.effective_originalyear());
      item->display_text = QString::number(year);
      item->sort_text = SortTextForNumber(year) + " ";
      break;
    }
    case GroupBy::Genre:{
      item->metadata.set_genre(s.genre());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      item->display_text = TextOrUnknown(s.genre());
      item->sort_text = SortTextForArtist(s.genre(), sort_skips_articles_);
      break;
    }
    case GroupBy::Composer:{
      item->metadata.set_composer(s.composer());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      item->display_text = TextOrUnknown(s.composer());
      item->sort_text = SortTextForArtist(s.composer(), sort_skips_articles_);
      break;
    }
    case GroupBy::Performer:{
      item->metadata.set_performer(s.performer());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      item->display_text = TextOrUnknown(s.performer());
      item->sort_text = SortTextForArtist(s.performer(), sort_skips_articles_);
      break;
    }
    case GroupBy::Grouping:{
      item->metadata.set_grouping(s.grouping());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      item->display_text = TextOrUnknown(s.grouping());
      item->sort_text = SortTextForArtist(s.grouping(), sort_skips_articles_);
      break;
    }
    case GroupBy::FileType:{
      item->metadata.set_filetype(s.filetype());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      item->display_text = s.TextForFiletype();
      item->sort_text = s.TextForFiletype();
      break;
    }
    case GroupBy::Format:{
      item->metadata.set_filetype(s.filetype());
      item->metadata.set_samplerate(s.samplerate());
      item->metadata.set_bitdepth(s.bitdepth());
      QString key = ContainerKey(group_by, separate_albums_by_grouping, s);
      item->key.append(key);
      item->display_text = key;
      item->sort_text = key;
      break;
    }
    case GroupBy::Samplerate:{
      item->metadata.set_samplerate(s.samplerate());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      const int samplerate = std::max(0, s.samplerate());
      item->display_text = QString::number(samplerate);
      item->sort_text = SortTextForNumber(samplerate) + " ";
      break;
    }
    case GroupBy::Bitdepth:{
      item->metadata.set_bitdepth(s.bitdepth());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      const int bitdepth = std::max(0, s.bitdepth());
      item->display_text = QString::number(bitdepth);
      item->sort_text = SortTextForNumber(bitdepth) + " ";
      break;
    }
    case GroupBy::Bitrate:{
      item->metadata.set_bitrate(s.bitrate());
      item->key.append(ContainerKey(group_by, separate_albums_by_grouping, s));
      const int bitrate = std::max(0, s.bitrate());
      item->display_text = QString::number(bitrate);
      item->sort_text = SortTextForNumber(bitrate) + " ";
      break;
    }
    case GroupBy::None:
    case GroupBy::GroupByCount:{
      item->metadata = s;
      item->key.append(TextOrUnknown(s.title()));
      item->display_text = s.TitleWithCompilationArtist();
      if (item->container_level == 1 && !IsAlbumGroupBy(group_by_[0])) {
        item->sort_text = SortText(s.title());
      }
      else {
        item->sort_text = SortTextForSong(s);
      }
      break;
    }
  }

  FinishItem(group_by, signal, create_divider, parent, item);

  return item;

}

void CollectionModel::FinishItem(const GroupBy group_by, const bool signal, const bool create_divider, CollectionItem *parent, CollectionItem *item) {

  if (signal) {
    endInsertRows();
  }

  // Create the divider entry if we're supposed to
  if (create_divider && show_dividers_) {
    QString divider_key = DividerKey(group_by, item);
    if (!divider_key.isEmpty()) {
      item->sort_text.prepend(divider_key + " ");
    }

    if (!divider_key.isEmpty() && !divider_nodes_.contains(divider_key)) {
      if (signal) {
        beginInsertRows(ItemToIndex(parent), static_cast<int>(parent->children.count()), static_cast<int>(parent->children.count()));
      }

      CollectionItem *divider = new CollectionItem(CollectionItem::Type_Divider, root_);
      divider->key = divider_key;
      divider->display_text = DividerDisplayText(group_by, divider_key);
      divider->sort_text = divider_key + "  ";

      divider_nodes_[divider_key] = divider;

      if (signal) {
        endInsertRows();
      }
    }
  }

}

QString CollectionModel::TextOrUnknown(const QString &text) {

  if (text.isEmpty()) return tr("Unknown");
  return text;

}

QString CollectionModel::PrettyYearAlbum(const int year, const QString &album) {

  if (year <= 0) return TextOrUnknown(album);
  return QString::number(year) + " - " + TextOrUnknown(album);

}

QString CollectionModel::PrettyAlbumDisc(const QString &album, const int disc) {

  if (disc <= 0 || Song::AlbumContainsDisc(album)) return TextOrUnknown(album);
  else return TextOrUnknown(album) + " - (Disc " + QString::number(disc) + ")";

}

QString CollectionModel::PrettyYearAlbumDisc(const int year, const QString &album, const int disc) {

  QString str;

  if (year <= 0) str = TextOrUnknown(album);
  else str = QString::number(year) + " - " + TextOrUnknown(album);

  if (!Song::AlbumContainsDisc(album) && disc > 0) str += " - (Disc " + QString::number(disc) + ")";

  return str;

}

QString CollectionModel::PrettyDisc(const int disc) {

  return "Disc " + QString::number(std::max(1, disc));

}

QString CollectionModel::SortText(QString text) {

  if (text.isEmpty()) {
    text = " unknown";
  }
  else {
    text = text.toLower();
  }
  text = text.remove(QRegularExpression("[^\\w ]", QRegularExpression::UseUnicodePropertiesOption));

  return text;

}

QString CollectionModel::SortTextForArtist(QString artist, const bool skip_articles) {

  artist = SortText(artist);

  if (skip_articles) {
    for (const auto &i : Song::kArticles) {
      if (artist.startsWith(i)) {
        qint64 ilen = i.length();
        artist = artist.right(artist.length() - ilen) + ", " + i.left(ilen - 1);
        break;
      }
    }
  }

  return artist;

}

QString CollectionModel::SortTextForNumber(const int number) {

  return QString("%1").arg(number, 4, 10, QChar('0'));
}

QString CollectionModel::SortTextForYear(const int year) {

  QString str = QString::number(year);
  return QString("0").repeated(qMax(0, 4 - str.length())) + str;

}

QString CollectionModel::SortTextForBitrate(const int bitrate) {

  QString str = QString::number(bitrate);
  return QString("0").repeated(qMax(0, 3 - str.length())) + str;

}

QString CollectionModel::SortTextForSong(const Song &song) {

  QString ret = QString::number(std::max(0, song.disc()) * 1000 + std::max(0, song.track()));
  ret.prepend(QString("0").repeated(6 - ret.length()));
  ret.append(song.url().toString());
  return ret;

}

Qt::ItemFlags CollectionModel::flags(const QModelIndex &idx) const {

  switch (IndexToItem(idx)->type) {
    case CollectionItem::Type_Song:
    case CollectionItem::Type_Container:
      return Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled;
    case CollectionItem::Type_Divider:
    case CollectionItem::Type_Root:
    case CollectionItem::Type_LoadingIndicator:
    default:
      return Qt::ItemIsEnabled;
  }

}

QStringList CollectionModel::mimeTypes() const {
  return QStringList() << "text/uri-list";
}

QMimeData *CollectionModel::mimeData(const QModelIndexList &indexes) const {

  if (indexes.isEmpty()) return nullptr;

  SongMimeData *data = new SongMimeData;
  QList<QUrl> urls;
  QSet<int> song_ids;

  data->backend = backend_;

  for (const QModelIndex &idx : indexes) {
    GetChildSongs(IndexToItem(idx), &urls, &data->songs, &song_ids);
  }

  data->setUrls(urls);
  data->name_for_new_playlist_ = PlaylistManager::GetNameForNewPlaylist(data->songs);

  return data;

}

bool CollectionModel::CompareItems(const CollectionItem *a, const CollectionItem *b) const {

  QVariant left(data(a, CollectionModel::Role_SortText));
  QVariant right(data(b, CollectionModel::Role_SortText));

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  if (left.metaType().id() == QMetaType::Int)
#else
  if (left.type() == QVariant::Int)
#endif
    return left.toInt() < right.toInt();
  else
    return left.toString() < right.toString();

}

qint64 CollectionModel::MaximumCacheSize(QSettings *s, const char *size_id, const char *size_unit_id, const qint64 cache_size_default) {

  qint64 size = s->value(size_id, cache_size_default).toInt();
  int unit = s->value(size_unit_id, static_cast<int>(CollectionSettingsPage::CacheSizeUnit::MB)).toInt() + 1;

  do {
    size *= 1024;
    unit -= 1;
  } while (unit > 0);

  return size;

}

void CollectionModel::GetChildSongs(CollectionItem *item, QList<QUrl> *urls, SongList *songs, QSet<int> *song_ids) const {

  switch (item->type) {
    case CollectionItem::Type_Container: {
      QList<CollectionItem*> children = item->children;
      std::sort(children.begin(), children.end(), std::bind(&CollectionModel::CompareItems, this, std::placeholders::_1, std::placeholders::_2));

      for (CollectionItem *child : children) {
        GetChildSongs(child, urls, songs, song_ids);
      }
      break;
    }

    case CollectionItem::Type_Song:
      urls->append(item->metadata.url());
      if (!song_ids->contains(item->metadata.id())) {
        songs->append(item->metadata);
        song_ids->insert(item->metadata.id());
      }
      break;

    default:
      break;
  }

}

SongList CollectionModel::GetChildSongs(const QModelIndexList &indexes) const {

  QList<QUrl> dontcare;
  SongList ret;
  QSet<int> song_ids;

  for (const QModelIndex &idx : indexes) {
    GetChildSongs(IndexToItem(idx), &dontcare, &ret, &song_ids);
  }
  return ret;

}

SongList CollectionModel::GetChildSongs(const QModelIndex &idx) const {
  return GetChildSongs(QModelIndexList() << idx);
}

void CollectionModel::SetFilterMode(const CollectionFilterOptions::FilterMode filter_mode) {

  filter_options_.set_filter_mode(filter_mode);
  ScheduleReset();

}

void CollectionModel::SetFilterAge(const int filter_age) {

  filter_options_.set_max_age(filter_age);
  ScheduleReset();

}

void CollectionModel::SetGroupBy(const Grouping g, const std::optional<bool> separate_albums_by_grouping) {

  group_by_ = g;
  if (separate_albums_by_grouping) {
    separate_albums_by_grouping_ = separate_albums_by_grouping.value();
  }

  ScheduleReset();

  emit GroupingChanged(g, separate_albums_by_grouping_);

}

const CollectionModel::GroupBy &CollectionModel::Grouping::operator[](const int i) const {

  switch (i) {
    case 0: return first;
    case 1: return second;
    case 2: return third;
    default: break;
  }
  qLog(Error) << "CollectionModel::Grouping[] index out of range" << i;
  return first;

}

CollectionModel::GroupBy &CollectionModel::Grouping::operator[](const int i) {

  switch (i) {
    case 0: return first;
    case 1: return second;
    case 2: return third;
    default: break;
  }
  qLog(Error) << "CollectionModel::Grouping[] index out of range" << i;

  return first;

}

void CollectionModel::TotalSongCountUpdatedSlot(const int count) {

  total_song_count_ = count;
  emit TotalSongCountUpdated(count);

}

void CollectionModel::TotalArtistCountUpdatedSlot(const int count) {

  total_artist_count_ = count;
  emit TotalArtistCountUpdated(count);

}

void CollectionModel::TotalAlbumCountUpdatedSlot(const int count) {

  total_album_count_ = count;
  emit TotalAlbumCountUpdated(count);

}

void CollectionModel::ClearDiskCache() {
  if (sIconCache) sIconCache->clear();
}

void CollectionModel::ExpandAll(CollectionItem *item) const {

  if (!root_) return;

  if (!item) item = root_;
  for (CollectionItem *child : item->children) {
    ExpandAll(child);
  }

}

bool CollectionModel::IsCollectionMetadataEqual(const Song &song1, const Song &song2) {

  return song1.title() == song2.title() &&
         song1.album() == song2.album() &&
         song1.artist() == song2.artist() &&
         song1.albumartist() == song2.albumartist() &&
         song1.track() == song2.track() &&
         song1.disc() == song2.disc() &&
         song1.year() == song2.year() &&
         song1.originalyear() == song2.originalyear() &&
         song1.genre() == song2.genre() &&
         song1.compilation() == song2.compilation() &&
         song1.composer() == song2.composer() &&
         song1.performer() == song2.performer() &&
         song1.grouping() == song2.grouping() &&
         song1.bitrate() == song2.bitrate() &&
         song1.samplerate() == song2.samplerate() &&
         song1.bitdepth() == song2.bitdepth();
}

QDataStream &operator<<(QDataStream &s, const CollectionModel::Grouping g) {
  s << static_cast<quint32>(g.first) << static_cast<quint32>(g.second) << static_cast<quint32>(g.third);
  return s;
}

QDataStream &operator>>(QDataStream &s, CollectionModel::Grouping &g) {

  quint32 buf = 0;
  s >> buf;
  g.first = static_cast<CollectionModel::GroupBy>(buf);
  s >> buf;
  g.second = static_cast<CollectionModel::GroupBy>(buf);
  s >> buf;
  g.third = static_cast<CollectionModel::GroupBy>(buf);
  return s;

}

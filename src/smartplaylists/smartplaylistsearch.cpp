/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
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

#include <QString>
#include <QStringList>
#include <QDataStream>

#include "core/song.h"

#include "smartplaylistsearch.h"

SmartPlaylistSearch::SmartPlaylistSearch() : search_type_(SearchType::And), sort_type_(SortType::Random), sort_field_(SmartPlaylistSearchTerm::Field::Title), limit_(-1), first_item_(0) { Reset(); }

SmartPlaylistSearch::SmartPlaylistSearch(const SearchType type, const TermList &terms, const SortType sort_type, const SmartPlaylistSearchTerm::Field sort_field, const int limit)
    : search_type_(type),
      terms_(terms),
      sort_type_(sort_type),
      sort_field_(sort_field),
      limit_(limit),
      first_item_(0) {}

void SmartPlaylistSearch::Reset() {

  search_type_ = SearchType::And;
  terms_.clear();
  sort_type_ = SortType::Random;
  sort_field_ = SmartPlaylistSearchTerm::Field::Title;
  limit_ = -1;
  first_item_ = 0;

}

QString SmartPlaylistSearch::ToSql(const QString &songs_table) const {

  QString sql = QStringLiteral("SELECT ROWID,") + Song::kColumnSpec + QStringLiteral(" FROM ") + songs_table;

  // Add search terms
  QStringList where_clauses;
  QStringList term_where_clauses;
  term_where_clauses.reserve(terms_.count());
  for (const SmartPlaylistSearchTerm &term : terms_) {
    term_where_clauses << term.ToSql();
  }

  if (!terms_.isEmpty() && search_type_ != SearchType::All) {
    QString boolean_op = search_type_ == SearchType::And ? QStringLiteral(" AND ") : QStringLiteral(" OR ");
    where_clauses << QStringLiteral("(") + term_where_clauses.join(boolean_op) + QStringLiteral(")");
  }

  // Restrict the IDs of songs if we're making a dynamic playlist
  if (!id_not_in_.isEmpty()) {
    QString numbers;
    for (int id : id_not_in_) {
      numbers += (numbers.isEmpty() ? QLatin1String("") : QStringLiteral(",")) + QString::number(id);
    }
    where_clauses << QStringLiteral("(ROWID NOT IN (") + numbers + QStringLiteral("))");
  }

  // We never want to include songs that have been deleted,
  // but are still kept in the database in case the directory containing them has just been unmounted.
  where_clauses << QStringLiteral("unavailable = 0");

  if (!where_clauses.isEmpty()) {
    sql += QStringLiteral(" WHERE ") + where_clauses.join(QStringLiteral(" AND "));
  }

  // Add sort by
  if (sort_type_ == SortType::Random) {
    sql += QLatin1String(" ORDER BY random()");
  }
  else {
    sql += QStringLiteral(" ORDER BY ") + SmartPlaylistSearchTerm::FieldColumnName(sort_field_) + (sort_type_ == SortType::FieldAsc ? QStringLiteral(" ASC") : QStringLiteral(" DESC"));
  }

  // Add limit
  if (first_item_ > 0) {
    sql += QStringLiteral(" LIMIT %1 OFFSET %2").arg(limit_).arg(first_item_);
  }
  else if (limit_ != -1) {
    sql += QStringLiteral(" LIMIT ") + QString::number(limit_);
  }
  //qLog(Debug) << sql;

  return sql;

}

bool SmartPlaylistSearch::is_valid() const {

  if (search_type_ == SearchType::All) return true;
  return !terms_.isEmpty();

}

bool SmartPlaylistSearch::operator==(const SmartPlaylistSearch &other) const {

  return search_type_ == other.search_type_ &&
         terms_ == other.terms_ &&
         sort_type_ == other.sort_type_ &&
         sort_field_ == other.sort_field_ &&
         limit_ == other.limit_;
}

QDataStream &operator<<(QDataStream &s, const SmartPlaylistSearch &search) {

  s << search.terms_;
  s << static_cast<quint8>(search.sort_type_);
  s << static_cast<quint8>(search.sort_field_);
  s << static_cast<quint32>(search.limit_);
  s << static_cast<quint8>(search.search_type_);
  return s;

}

QDataStream &operator>>(QDataStream &s, SmartPlaylistSearch &search) {

  quint8 sort_type = 0, sort_field = 0, search_type = 0;
  qint32 limit = 0;

  s >> search.terms_ >> sort_type >> sort_field >> limit >> search_type;
  search.sort_type_ = static_cast<SmartPlaylistSearch::SortType>(sort_type);
  search.sort_field_ = static_cast<SmartPlaylistSearchTerm::Field>(sort_field);
  search.limit_ = limit;
  search.search_type_ = static_cast<SmartPlaylistSearch::SearchType>(search_type);

  return s;

}

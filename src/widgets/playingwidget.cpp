/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2013, Jonas Kvinge <jonas@strawbs.net>
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

#include <QtGlobal>
#include <QWidget>
#include <QList>
#include <QByteArray>
#include <QVariant>
#include <QString>
#include <QImage>
#include <QPixmap>
#include <QSize>
#include <QMenu>
#include <QMovie>
#include <QPainter>
#include <QPalette>
#include <QBrush>
#include <QSignalMapper>
#include <QTextDocument>
#include <QTimeLine>
#include <QAction>
#include <QActionGroup>
#include <QSettings>
#include <QtEvents>

#include "core/application.h"
#include "covermanager/albumcoverchoicecontroller.h"
#include "covermanager/albumcoverloader.h"
#include "covermanager/currentartloader.h"
#include "playingwidget.h"

using std::unique_ptr;

const char *PlayingWidget::kSettingsGroup = "PlayingWidget";

// Space between the cover and the details in small mode
const int PlayingWidget::kPadding = 2;

// Width of the transparent to black gradient above and below the text in large mode
const int PlayingWidget::kGradientHead = 40;
const int PlayingWidget::kGradientTail = 20;

// Maximum height of the cover in large mode, and offset between the bottom of the cover and bottom of the widget
const int PlayingWidget::kMaxCoverSize = 260;
const int PlayingWidget::kBottomOffset = 0;

// Border for large mode
const int PlayingWidget::kTopBorder = 4;

PlayingWidget::PlayingWidget(QWidget *parent)
    : QWidget(parent),
      app_(nullptr),
      album_cover_choice_controller_(nullptr),
      mode_(LargeSongDetails),
      menu_(new QMenu(this)),
      above_statusbar_action_(nullptr),
      fit_cover_width_action_(nullptr),
      enabled_(false),
      visible_(false),
      playing_(false),
      active_(false),
      small_ideal_height_(0),
      fit_width_(false),
      timeline_show_hide_(new QTimeLine(500, this)),
      timeline_fade_(new QTimeLine(1000, this)),
      details_(new QTextDocument(this)),
      pixmap_previous_track_opacity_(0.0),
      downloading_covers_(false) {

  SetHeight(0);

  // Load settings
  QSettings s;
  s.beginGroup(kSettingsGroup);
  mode_ = Mode(s.value("mode", LargeSongDetails).toInt());
  fit_width_ = s.value("fit_cover_width", false).toBool();
  s.endGroup();

  // Accept drops for setting album art
  setAcceptDrops(true);

  // Context menu
  QActionGroup *mode_group = new QActionGroup(this);
  QSignalMapper *mode_mapper = new QSignalMapper(this);
  connect(mode_mapper, SIGNAL(mapped(int)), SLOT(SetMode(int)));
  CreateModeAction(SmallSongDetails, tr("Small album cover"), mode_group, mode_mapper);
  CreateModeAction(LargeSongDetails, tr("Large album cover"), mode_group, mode_mapper);
  menu_->addActions(mode_group->actions());

  fit_cover_width_action_ = menu_->addAction(tr("Fit cover to width"));
  fit_cover_width_action_->setCheckable(true);
  fit_cover_width_action_->setEnabled(true);
  connect(fit_cover_width_action_, SIGNAL(toggled(bool)), SLOT(FitCoverWidth(bool)));
  fit_cover_width_action_->setChecked(fit_width_);
  menu_->addSeparator();

  // Animations
  connect(timeline_show_hide_, SIGNAL(frameChanged(int)), SLOT(SetHeight(int)));
  connect(timeline_fade_, SIGNAL(valueChanged(qreal)), SLOT(FadePreviousTrack(qreal)));
  timeline_fade_->setDirection(QTimeLine::Backward);  // 1.0 -> 0.0

  // add placeholder text to get the correct height
  if (mode_ == LargeSongDetails) {
    details_->setDefaultStyleSheet("p { font-size: small; font-weight: bold; }");
    details_->setHtml(QString("<p align=center><i></i><br/><br/></p>"));
  }

  UpdateHeight();

}

PlayingWidget::~PlayingWidget() {}

void PlayingWidget::SetApplication(Application *app, AlbumCoverChoiceController *album_cover_choice_controller) {

  app_ = app;
  connect(app_->current_art_loader(), SIGNAL(ArtLoaded(Song, QString, QImage)), SLOT(AlbumArtLoaded(Song, QString, QImage)));

  album_cover_choice_controller_ = album_cover_choice_controller;
  album_cover_choice_controller_->SetApplication(app_);
  QList<QAction*> cover_actions = album_cover_choice_controller_->GetAllActions();
  cover_actions.append(album_cover_choice_controller_->search_cover_auto_action());
  menu_->addActions(cover_actions);
  menu_->addSeparator();

  above_statusbar_action_ = menu_->addAction(tr("Show above status bar"));
  above_statusbar_action_->setCheckable(true);
  QSettings s;
  s.beginGroup(kSettingsGroup);
  above_statusbar_action_->setChecked(s.value("above_status_bar", false).toBool());
  s.endGroup();
  connect(above_statusbar_action_, SIGNAL(toggled(bool)), SLOT(ShowAboveStatusBar(bool)));

  connect(album_cover_choice_controller_, SIGNAL(AutomaticCoverSearchDone()), this, SLOT(AutomaticCoverSearchDone()));
  connect(album_cover_choice_controller_->search_cover_auto_action(), SIGNAL(triggered()), this, SLOT(SearchCoverAutomatically()));

}


void PlayingWidget::SetEnabled() {
  enabled_ = true;
  if (!visible_ && active_) SetVisible(true);
}

void PlayingWidget::SetDisabled() {
  enabled_ = false;
  if (visible_) SetVisible(false);
}

void PlayingWidget::SetVisible(bool visible) {

  if (timeline_show_hide_->state() == QTimeLine::Running) {
    if (timeline_show_hide_->direction() == QTimeLine::Backward && enabled_ && active_) {
      timeline_show_hide_->toggleDirection();
    }
    if (timeline_show_hide_->direction() == QTimeLine::Forward && (!enabled_ || !active_)) {
      timeline_show_hide_->toggleDirection();
    }
    return;
  }

  if (visible == visible_) return;  

  timeline_show_hide_->setFrameRange(0, total_height_);
  timeline_show_hide_->setDirection(visible ? QTimeLine::Forward : QTimeLine::Backward);
  timeline_show_hide_->start();

}

void PlayingWidget::set_ideal_height(int height) {

  small_ideal_height_ = height;
  UpdateHeight();

}

QSize PlayingWidget::sizeHint() const {
  return QSize(cover_loader_options_.desired_height_, total_height_);
}

void PlayingWidget::CreateModeAction(Mode mode, const QString &text, QActionGroup *group, QSignalMapper *mapper) {

  QAction *action = new QAction(text, group);
  action->setCheckable(true);
  mapper->setMapping(action, mode);
  connect(action, SIGNAL(triggered()), mapper, SLOT(map()));

  if (mode == mode_) action->setChecked(true);

}

void PlayingWidget::SetMode(int mode) {

  mode_ = Mode(mode);

  if (mode_ == SmallSongDetails) {
    fit_cover_width_action_->setEnabled(false);
  }
  else {
    fit_cover_width_action_->setEnabled(true);
  }

  UpdateHeight();
  UpdateDetailsText();
  update();

  QSettings s;
  s.beginGroup(kSettingsGroup);
  s.setValue("mode", mode_);
  s.endGroup();

}

void PlayingWidget::FitCoverWidth(bool fit) {

  fit_width_ = fit;
  UpdateHeight();
  update();

  QSettings s;
  s.beginGroup(kSettingsGroup);
  s.setValue("fit_cover_width", fit_width_);
}

void PlayingWidget::ShowAboveStatusBar(bool above) {
  QSettings s;
  s.beginGroup(kSettingsGroup);
  s.setValue("above_status_bar", above);
  emit ShowAboveStatusBarChanged(above);
  s.endGroup();
}

void PlayingWidget::Playing() {
}

void PlayingWidget::Stopped() {
  playing_ = false;
  active_ = false;
  song_playing_ = song_empty_;
  song_ = song_empty_;
  SetVisible(false);
}

void PlayingWidget::Error() {
  active_ = false;
}

void PlayingWidget::SongChanged(const Song &song) {
  playing_ = true;
  song_playing_ = song;
  song_ = song;
}

void PlayingWidget::AlbumArtLoaded(const Song &song, const QString &, const QImage &image) {

  if (!playing_ || song.id() != song_playing_.id() || song.url() != song_playing_.url() || song.effective_albumartist() != song_playing_.effective_albumartist() || song.effective_album() != song_playing_.effective_album() || song.title() != song_playing_.title()) return;
  if (timeline_fade_->state() == QTimeLine::Running && image == image_original_) return;

  active_ = true;
  downloading_covers_ = false;
  song_ = song;
  SetImage(image);
  GetCoverAutomatically();

}

void PlayingWidget::SetImage(const QImage &image) {

  if (enabled_ && visible_ && active_) {
    // Cache the current pixmap so we can fade between them
    QSize psize(size());
    if (size().height() <= 0) psize.setHeight(total_height_);
    pixmap_previous_track_ = QPixmap(psize);
    pixmap_previous_track_.fill(palette().background().color());
    pixmap_previous_track_opacity_ = 1.0;
    QPainter p(&pixmap_previous_track_);
    DrawContents(&p);
    p.end();
  }
  else { pixmap_previous_track_ = QPixmap(); }

  image_original_ = image;
  UpdateDetailsText();
  ScaleCover();

  if (enabled_ && active_) {
    SetVisible(true);
    // Were we waiting for this cover to load before we started fading?
    if (!pixmap_previous_track_.isNull()) {
      timeline_fade_->stop();
      timeline_fade_->start();
    }
  }

}

void PlayingWidget::ScaleCover() {
  pixmap_cover_ = QPixmap::fromImage(AlbumCoverLoader::ScaleAndPad(cover_loader_options_, image_original_));
  update();
}

void PlayingWidget::SetHeight(int height) {

  setMaximumHeight(height);
  update();

  if (height >= total_height_) visible_ = true;
  if (height <= 0) visible_ = false;

  if (timeline_show_hide_->state() == QTimeLine::Running) {
    if (timeline_show_hide_->direction() == QTimeLine::Backward && enabled_ && active_) {
      timeline_show_hide_->toggleDirection();
    }
    if (timeline_show_hide_->direction() == QTimeLine::Forward && (!enabled_ || !active_)) {
      timeline_show_hide_->toggleDirection();
    }
  }

}

void PlayingWidget::UpdateHeight() {

  switch (mode_) {
    case SmallSongDetails:
      cover_loader_options_.desired_height_ = small_ideal_height_;
      total_height_ = small_ideal_height_;
      break;
    case LargeSongDetails:
      if (fit_width_) cover_loader_options_.desired_height_ = width();
      else cover_loader_options_.desired_height_ = qMin(kMaxCoverSize, width());
      total_height_ = kTopBorder + cover_loader_options_.desired_height_ + kBottomOffset + details_->size().height();
      break;
  }

  // Update the animation settings and resize the widget now if we're visible
  timeline_show_hide_->setFrameRange(0, total_height_);
  if (visible_ && active_ && timeline_show_hide_->state() != QTimeLine::Running) setMaximumHeight(total_height_);

  // Re-scale the current image
  if (song_.is_valid()) {
    ScaleCover();
  }

  // Tell Qt we've changed size
  updateGeometry();

}

void PlayingWidget::UpdateDetailsText() {

  QString html("");
  details_->setDefaultStyleSheet("p { font-size: small; font-weight: bold; }");
  switch (mode_) {
    case SmallSongDetails:
      details_->setTextWidth(-1);
      html += "<p>";
      break;
    case LargeSongDetails:
      details_->setTextWidth(cover_loader_options_.desired_height_);
      html += "<p align=center>";
      break;
  }

  html += QString("%1<br/>%2<br/>%3").arg(song_.PrettyTitle().toHtmlEscaped(), song_.artist().toHtmlEscaped(), song_.album().toHtmlEscaped());

  html += "</p>";
  details_->setHtml(html);

  // if something spans multiple lines the height needs to change
  if (mode_ == LargeSongDetails) UpdateHeight();

}

void PlayingWidget::paintEvent(QPaintEvent *e) {

  QPainter p(this);

  DrawContents(&p);

  // Draw the previous track's image if we're fading
  if (!pixmap_previous_track_.isNull()) {
    p.setOpacity(pixmap_previous_track_opacity_);
    p.drawPixmap(0, 0, pixmap_previous_track_);
  }
}

void PlayingWidget::DrawContents(QPainter *p) {

  switch (mode_) {
    case SmallSongDetails:
      // Draw the cover
      p->drawPixmap(0, 0, small_ideal_height_, small_ideal_height_, pixmap_cover_);
      if (downloading_covers_) {
        p->drawPixmap(small_ideal_height_ - 18, 6, 16, 16, spinner_animation_->currentPixmap());
      }

      // Draw the details
      p->translate(small_ideal_height_ + kPadding, 0);
      details_->drawContents(p);
      p->translate(-small_ideal_height_ - kPadding, 0);
      break;

    case LargeSongDetails:
      // Work out how high the text is going to be
      const int text_height = details_->size().height();
      const int cover_size = fit_width_ ? width() : qMin(kMaxCoverSize, width());
      const int x_offset = (width() - cover_loader_options_.desired_height_) / 2;

      // Draw the cover
      p->drawPixmap(x_offset, kTopBorder, cover_size, cover_size, pixmap_cover_);
      if (downloading_covers_) {
        p->drawPixmap(x_offset + 45, 35, 16, 16, spinner_animation_->currentPixmap());
      }

      // Draw the text below
      p->translate(x_offset, height() - text_height);
      details_->drawContents(p);
      p->translate(-x_offset, -height() + text_height);

      break;
  }

}

void PlayingWidget::FadePreviousTrack(qreal value) {

  if (!visible_) return;

  pixmap_previous_track_opacity_ = value;
  if (qFuzzyCompare(pixmap_previous_track_opacity_, qreal(0.0))) {
    pixmap_previous_track_ = QPixmap();
  }

  update();

}

void PlayingWidget::resizeEvent(QResizeEvent* e) {

  //if (visible_ && e->oldSize() != e->size()) {
  if (e->oldSize() != e->size()) {
    if (mode_ == LargeSongDetails) {
      UpdateHeight();
      UpdateDetailsText();
    }
  }

}

void PlayingWidget::contextMenuEvent(QContextMenuEvent* e) {

  // show the menu
  menu_->popup(mapToGlobal(e->pos()));
}

void PlayingWidget::mouseReleaseEvent(QMouseEvent*) {
  // Same behaviour as right-click > Show Fullsize

}

void PlayingWidget::dragEnterEvent(QDragEnterEvent *e) {

  if (AlbumCoverChoiceController::CanAcceptDrag(e)) {
    e->acceptProposedAction();
  }

  QWidget::dragEnterEvent(e);

}

void PlayingWidget::dropEvent(QDropEvent *e) {

  album_cover_choice_controller_->SaveCover(&song_, e);

  QWidget::dropEvent(e);

}

void PlayingWidget::GetCoverAutomatically() {

  // Search for cover automatically?
  bool search =
               album_cover_choice_controller_->search_cover_auto_action()->isChecked() &&
               !song_.has_manually_unset_cover() &&
               song_.art_automatic().isEmpty() &&
               song_.art_manual().isEmpty() &&
               !song_.effective_albumartist().isEmpty() &&
               !song_.effective_album().isEmpty();

  if (search) {
    downloading_covers_ = true;
    // This is done in mainwindow instead to avoid searching multiple times (ContextView & PlayingWidget)
    // album_cover_choice_controller_->SearchCoverAutomatically(song_);

    // Show a spinner animation
    spinner_animation_.reset(new QMovie(":/pictures/spinner.gif", QByteArray(), this));
    connect(spinner_animation_.get(), SIGNAL(updated(const QRect&)), SLOT(update()));
    spinner_animation_->start();
    update();
  }

}

void PlayingWidget::AutomaticCoverSearchDone() {

  downloading_covers_ = false;
  spinner_animation_.reset();
  update();

}

void PlayingWidget::SearchCoverAutomatically() {
  GetCoverAutomatically();
}

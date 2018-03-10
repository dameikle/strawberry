/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2012, David Sansome <me@davidsansome.com>
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

#include "application.h"

#include "config.h"

#include "core/appearance.h"
#include "core/database.h"
#include "core/lazy.h"
#include "core/player.h"
#include "core/tagreaderclient.h"
#include "core/taskmanager.h"
#include "engine/enginetype.h"
#include "engine/enginedevice.h"
#include "device/devicemanager.h"
#include "collection/collectionbackend.h"
#include "collection/collection.h"
#include "playlist/playlistbackend.h"
#include "playlist/playlistmanager.h"
#include "covermanager/albumcoverloader.h"
#include "covermanager/coverproviders.h"
#include "covermanager/currentartloader.h"
#ifdef HAVE_LIBLASTFM
  #include "covermanager/lastfmcoverprovider.h"
#endif  // HAVE_LIBLASTFM
#include "covermanager/amazoncoverprovider.h"
#include "covermanager/discogscoverprovider.h"
#include "covermanager/musicbrainzcoverprovider.h"

bool Application::kIsPortable = false;

class ApplicationImpl {
 public:
  ApplicationImpl(Application *app) :
	tag_reader_client_([=]() {
          TagReaderClient *client = new TagReaderClient(app);
          app->MoveToNewThread(client);
          client->Start();
          return client;
        }),
	database_([=]() {
          Database *db = new Database(app, app);
          app->MoveToNewThread(db);
          DoInAMinuteOrSo(db, SLOT(DoBackup()));
          return db;
	}),
        appearance_([=]() { return new Appearance(app); }),
        task_manager_([=]() { return new TaskManager(app); }),
        player_([=]() { return new Player(app, app); }),
        enginedevice_([=]() { return new EngineDevice(app); }),
        device_manager_([=]() { return new DeviceManager(app, app); }),
        collection_([=]() { return new Collection(app, app); }),
        playlist_backend_([=]() {
          PlaylistBackend *backend = new PlaylistBackend(app, app);
          app->MoveToThread(backend, database_->thread());
          return backend;
        }),
        playlist_manager_([=]() { return new PlaylistManager(app); }),
        cover_providers_([=]() {
          CoverProviders *cover_providers = new CoverProviders(app);
          // Initialize the repository of cover providers.
        #ifdef HAVE_LIBLASTFM
          cover_providers->AddProvider(new LastFmCoverProvider(app));
        #endif
          cover_providers->AddProvider(new AmazonCoverProvider(app));
	  cover_providers->AddProvider(new DiscogsCoverProvider(app));
          cover_providers->AddProvider(new MusicbrainzCoverProvider(app));
          return cover_providers;
        }),
        album_cover_loader_([=]() {
          AlbumCoverLoader *loader = new AlbumCoverLoader(app);
          app->MoveToNewThread(loader);
          return loader;
        }),
        current_art_loader_([=]() { return new CurrentArtLoader(app, app); })
  { }

  Lazy<TagReaderClient> tag_reader_client_;
  Lazy<Database> database_;
  Lazy<Appearance> appearance_;
  Lazy<TaskManager> task_manager_;
  Lazy<Player> player_;
  Lazy<EngineDevice> enginedevice_;
  Lazy<DeviceManager> device_manager_;
  Lazy<Collection> collection_;
  Lazy<PlaylistBackend> playlist_backend_;
  Lazy<PlaylistManager> playlist_manager_;
  Lazy<CoverProviders> cover_providers_;
  Lazy<AlbumCoverLoader> album_cover_loader_;
  Lazy<CurrentArtLoader> current_art_loader_;

};

Application::Application(QObject *parent)
    : QObject(parent), p_(new ApplicationImpl(this)) {

  enginedevice()->Init();
  collection()->Init();
  tag_reader_client();

}

Application::~Application() {

  // It's important that the device manager is deleted before the database.
  // Deleting the database deletes all objects that have been created in its
  // thread, including some device collection backends.
  p_->device_manager_.reset();

  for (QThread *thread : threads_) {
    thread->quit();
  }

  for (QThread *thread : threads_) {
    thread->wait();
  }
}

void Application::MoveToNewThread(QObject *object) {

  QThread *thread = new QThread(this);

  MoveToThread(object, thread);

  thread->start();
  threads_ << thread;
}

void Application::MoveToThread(QObject *object, QThread *thread) {
  object->setParent(nullptr);
  object->moveToThread(thread);
}

void Application::AddError(const QString& message) { emit ErrorAdded(message); }

QString Application::language_without_region() const {
  const int underscore = language_name_.indexOf('_');
  if (underscore != -1) {
    return language_name_.left(underscore);
  }
  return language_name_;
}

void Application::ReloadSettings() { emit SettingsChanged(); }

void Application::OpenSettingsDialogAtPage(SettingsDialog::Page page) {
  emit SettingsDialogRequested(page);
}

AlbumCoverLoader *Application::album_cover_loader() const {
  return p_->album_cover_loader_.get();
}

Appearance *Application::appearance() const { return p_->appearance_.get(); }

CoverProviders *Application::cover_providers() const {
  return p_->cover_providers_.get();
}

CurrentArtLoader *Application::current_art_loader() const {
  return p_->current_art_loader_.get();
}

Database *Application::database() const { return p_->database_.get(); }

DeviceManager *Application::device_manager() const {
  return p_->device_manager_.get();
}

Collection *Application::collection() const { return p_->collection_.get(); }

CollectionBackend *Application::collection_backend() const {
  return collection()->backend();
}

CollectionModel *Application::collection_model() const { return collection()->model(); }

Player *Application::player() const { return p_->player_.get(); }

PlaylistBackend *Application::playlist_backend() const {
  return p_->playlist_backend_.get();
}

PlaylistManager *Application::playlist_manager() const {
  return p_->playlist_manager_.get();
}

TagReaderClient *Application::tag_reader_client() const {
  return p_->tag_reader_client_.get();
}

TaskManager *Application::task_manager() const {
  return p_->task_manager_.get();
}

EngineDevice *Application::enginedevice() const {
  //qLog(Debug) << __PRETTY_FUNCTION__;
  return p_->enginedevice_.get();
}
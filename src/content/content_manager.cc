/*MT*

    MediaTomb - http://www.mediatomb.cc/

    content_manager.cc - this file is part of MediaTomb.

    Copyright (C) 2005 Gena Batyan <bgeradz@mediatomb.cc>,
                       Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>

    Copyright (C) 2006-2010 Gena Batyan <bgeradz@mediatomb.cc>,
                            Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>,
                            Leonhard Wimmer <leo@mediatomb.cc>

    MediaTomb is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation.

    MediaTomb is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    version 2 along with MediaTomb; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.

    $Id$
*/

/// \file content_manager.cc

#include "content_manager.h" // API

#include <cerrno>
#include <chrono>
#include <cstring>
#include <regex>

#include "config/config_manager.h"
#include "config/directory_tweak.h"
#include "database/database.h"
#include "layout/builtin_layout.h"
#include "metadata/metadata_handler.h"
#include "update_manager.h"
#include "util/mime.h"
#include "util/process.h"
#include "util/string_converter.h"
#include "util/timer.h"
#include "util/tools.h"
#include "web/session_manager.h"

#ifdef HAVE_JS
#include "layout/js_layout.h"
#endif

#ifdef HAVE_LASTFMLIB
#include "onlineservice/lastfm_scrobbler.h"
#endif

#ifdef SOPCAST
#include "onlineservice/sopcast_service.h"
#endif

#ifdef ATRAILERS
#include "onlineservice/atrailers_service.h"
#endif

#ifdef ONLINE_SERVICES
#include "onlineservice/task_processor.h"
#endif

#ifdef HAVE_JS
#include "scripting/scripting_runtime.h"
#endif

ContentManager::ContentManager(const std::shared_ptr<Context>& context,
    const std::shared_ptr<Server>& server, std::shared_ptr<Timer> timer)
    : config(context->getConfig())
    , mime(context->getMime())
    , database(context->getDatabase())
    , session_manager(context->getSessionManager())
    , context(context)
    , timer(std::move(timer))
{
    taskID = 1;
    working = false;
    shutdownFlag = false;
    layout_enabled = false;

    update_manager = std::make_shared<UpdateManager>(config, database, server);
#ifdef ONLINE_SERVICES
    task_processor = std::make_shared<TaskProcessor>(config);
#endif
#ifdef HAVE_JS
    scripting_runtime = std::make_shared<ScriptingRuntime>();
#endif
#ifdef HAVE_LASTFMLIB
    last_fm = std::make_shared<LastFm>(context);
#endif

    mimetype_contenttype_map = config->getDictionaryOption(CFG_IMPORT_MAPPINGS_MIMETYPE_TO_CONTENTTYPE_LIST);
}

void ContentManager::run()
{
#ifdef ONLINE_SERVICES
    task_processor->run();
#endif
    update_manager->run();
#ifdef HAVE_LASTFMLIB
    last_fm->run();
#endif
    threadRunner = std::make_unique<ThreadRunner<std::condition_variable_any, std::recursive_mutex>>("ContentTaskThread", ContentManager::staticThreadProc, this, config);

    if (!threadRunner->isAlive()) {
        throw_std_runtime_error("Could not start task thread");
    }

    auto config_timed_list = config->getAutoscanListOption(CFG_IMPORT_AUTOSCAN_TIMED_LIST);
    for (size_t i = 0; i < config_timed_list->size(); i++) {
        auto dir = config_timed_list->get(i);
        if (dir != nullptr) {
            fs::path path = dir->getLocation();
            if (fs::is_directory(path)) {
                dir->setObjectID(ensurePathExistence(path));
            }
        }
    }

    database->updateAutoscanList(ScanMode::Timed, config_timed_list);
    autoscan_timed = database->getAutoscanList(ScanMode::Timed);

    auto self = shared_from_this();
#ifdef HAVE_INOTIFY
    inotify = std::make_unique<AutoscanInotify>(self);

    if (config->getBoolOption(CFG_IMPORT_AUTOSCAN_USE_INOTIFY)) {
        auto config_inotify_list = config->getAutoscanListOption(CFG_IMPORT_AUTOSCAN_INOTIFY_LIST);
        for (size_t i = 0; i < config_inotify_list->size(); i++) {
            auto dir = config_inotify_list->get(i);
            if (dir != nullptr) {
                fs::path path = dir->getLocation();
                if (fs::is_directory(path)) {
                    dir->setObjectID(ensurePathExistence(path));
                }
            }
        }

        database->updateAutoscanList(ScanMode::INotify, config_inotify_list);
        autoscan_inotify = database->getAutoscanList(ScanMode::INotify);
    } else {
        // make an empty list so we do not have to do extra checks on shutdown
        autoscan_inotify = std::make_shared<AutoscanList>(database);
    }

    // Start INotify thread
    inotify->run();
#endif

    std::string layout_type = config->getOption(CFG_IMPORT_SCRIPTING_VIRTUAL_LAYOUT_TYPE);
    if ((layout_type == "builtin") || (layout_type == "js"))
        layout_enabled = true;

#ifdef ONLINE_SERVICES
    online_services = std::make_unique<OnlineServiceList>();

#ifdef SOPCAST
    if (config->getBoolOption(CFG_ONLINE_CONTENT_SOPCAST_ENABLED)) {
        try {
            auto sc = std::make_shared<SopCastService>(self);

            int i = config->getIntOption(CFG_ONLINE_CONTENT_SOPCAST_REFRESH);
            sc->setRefreshInterval(i);

            i = config->getIntOption(CFG_ONLINE_CONTENT_SOPCAST_PURGE_AFTER);
            sc->setItemPurgeInterval(i);

            if (config->getBoolOption(CFG_ONLINE_CONTENT_SOPCAST_UPDATE_AT_START))
                i = CFG_DEFAULT_UPDATE_AT_START;

            auto sc_param = std::make_shared<Timer::Parameter>(Timer::Parameter::IDOnlineContent, OS_SopCast);
            sc->setTimerParameter(sc_param);
            online_services->registerService(sc);
            if (i > 0) {
                timer->addTimerSubscriber(this, i, sc->getTimerParameter(), true);
            }
        } catch (const std::runtime_error& ex) {
            log_error("Could not setup SopCast: {}", ex.what());
        }
    }
#endif // SOPCAST

#ifdef ATRAILERS
    if (config->getBoolOption(CFG_ONLINE_CONTENT_ATRAILERS_ENABLED)) {
        try {
            auto at = std::make_shared<ATrailersService>(self);

            int i = config->getIntOption(CFG_ONLINE_CONTENT_ATRAILERS_REFRESH);
            at->setRefreshInterval(i);

            i = config->getIntOption(CFG_ONLINE_CONTENT_ATRAILERS_PURGE_AFTER);
            at->setItemPurgeInterval(i);
            if (config->getBoolOption(CFG_ONLINE_CONTENT_ATRAILERS_UPDATE_AT_START))
                i = CFG_DEFAULT_UPDATE_AT_START;

            auto at_param = std::make_shared<Timer::Parameter>(Timer::Parameter::IDOnlineContent, OS_ATrailers);
            at->setTimerParameter(at_param);
            online_services->registerService(at);
            if (i > 0) {
                timer->addTimerSubscriber(this, i, at->getTimerParameter(), true);
            }
        } catch (const std::runtime_error& ex) {
            log_error("Could not setup Apple Trailers: {}", ex.what());
        }
    }
#endif // ATRAILERS

#endif // ONLINE_SERVICES

    if (layout_enabled)
        initLayout();

#ifdef HAVE_JS
    initJS();
#endif

    autoscan_timed->notifyAll(this);

#ifdef HAVE_INOTIFY
    if (config->getBoolOption(CFG_IMPORT_AUTOSCAN_USE_INOTIFY)) {
        /// \todo change this (we need a new autoscan architecture)
        for (size_t i = 0; i < autoscan_inotify->size(); i++) {
            std::shared_ptr<AutoscanDirectory> adir = autoscan_inotify->get(i);
            if (adir == nullptr) {
                continue;
            }

            inotify->monitor(adir);
            auto param = std::make_shared<Timer::Parameter>(Timer::Parameter::timer_param_t::IDAutoscan, adir->getScanID());
            log_debug("Adding one-shot inotify scan");
            timer->addTimerSubscriber(this, 60, param, true);
        }
    }
#endif

    for (size_t i = 0; i < autoscan_timed->size(); i++) {
        std::shared_ptr<AutoscanDirectory> adir = autoscan_timed->get(i);
        auto param = std::make_shared<Timer::Parameter>(Timer::Parameter::timer_param_t::IDAutoscan, adir->getScanID());
        log_debug("Adding timed scan with interval {}", adir->getInterval());
        timer->addTimerSubscriber(this, adir->getInterval(), param, false);
    }
}

ContentManager::~ContentManager() { log_debug("ContentManager destroyed"); }

void ContentManager::registerExecutor(const std::shared_ptr<Executor>& exec)
{
    auto lock = threadRunner->lockGuard("registerExecutor");
    process_list.push_back(exec);
}

void ContentManager::unregisterExecutor(const std::shared_ptr<Executor>& exec)
{
    // when shutting down we will kill the transcoding processes,
    // which if given enough time will get a close in the io handler and
    // will try to unregister themselves - this would mess up the
    // transcoding_processes list
    // since we are anyway shutting down we can ignore the unregister call
    // and go through the list, ensuring that no zombie stays alive :>
    if (shutdownFlag)
        return;

    auto lock = threadRunner->lockGuard("unregisterExecutor");

    process_list.erase(std::remove_if(process_list.begin(), process_list.end(), [&](const auto& e) { return e == exec; }), process_list.end());
}

void ContentManager::timerNotify(std::shared_ptr<Timer::Parameter> parameter)
{
    if (parameter == nullptr)
        return;

    if (parameter->whoami() == Timer::Parameter::IDAutoscan) {
        std::shared_ptr<AutoscanDirectory> adir = autoscan_timed->get(parameter->getID());

        // do not rescan while other scans are still active
        if (adir == nullptr || adir->getActiveScanCount() > 0 || adir->getTaskCount() > 0)
            return;

        rescanDirectory(adir, adir->getObjectID());
    }
#ifdef ONLINE_SERVICES
    else if (parameter->whoami() == Timer::Parameter::IDOnlineContent) {
        fetchOnlineContent(service_type_t(parameter->getID()));
    }
#endif // ONLINE_SERVICES
}

void ContentManager::shutdown()
{
    log_debug("start");
    auto lock = threadRunner->uniqueLock();
    log_debug("updating last_modified data for autoscan in database...");
    autoscan_timed->updateLMinDB();

#ifdef HAVE_JS
    destroyJS();
#endif
    destroyLayout();

#ifdef HAVE_INOTIFY
    if (autoscan_inotify) {
        // update modification time for database
        for (size_t i = 0; i < autoscan_inotify->size(); i++) {
            log_debug("AutoScanDir {}", i);
            std::shared_ptr<AutoscanDirectory> dir = autoscan_inotify->get(i);
            if (dir != nullptr) {
                if (fs::is_directory(dir->getLocation())) {
                    auto t = getLastWriteTime(dir->getLocation());
                    dir->setCurrentLMT(dir->getLocation(), t);
                }
                dir->updateLMT();
            }
        }
        autoscan_inotify->updateLMinDB();

        autoscan_inotify = nullptr;
        inotify = nullptr;
    }
#endif

    shutdownFlag = true;

    for (const auto& exec : process_list) {
        if (exec != nullptr)
            exec->kill();
    }

    log_debug("signalling...");
    threadRunner->notify();
    lock.unlock();
    log_debug("waiting for thread...");

    threadRunner->join();

#ifdef HAVE_LASTFMLIB
    last_fm->shutdown();
    last_fm = nullptr;
#endif
#ifdef HAVE_JS
    scripting_runtime = nullptr;
#endif
#ifdef ONLINE_SERVICES
    task_processor->shutdown();
    task_processor = nullptr;
#endif
    update_manager->shutdown();
    update_manager = nullptr;

    log_debug("end");
}

std::shared_ptr<GenericTask> ContentManager::getCurrentTask()
{
    auto lock = threadRunner->lockGuard("getCurrentTask");

    auto task = currentTask;
    return task;
}

std::deque<std::shared_ptr<GenericTask>> ContentManager::getTasklist()
{
    auto lock = threadRunner->lockGuard("getTasklist");

    std::deque<std::shared_ptr<GenericTask>> taskList;
#ifdef ONLINE_SERVICES
    taskList = task_processor->getTasklist();
#endif
    auto t = getCurrentTask();

    // if there is no current task, then the queues are empty
    // and we do not have to allocate the array
    if (t == nullptr)
        return taskList;

    taskList.push_back(t);
    std::copy_if(taskQueue1.begin(), taskQueue1.end(), std::back_inserter(taskList), [](const auto& task) { return task->isValid(); });

    for (const auto& task : taskQueue2) {
        if (task->isValid())
            taskList.clear();
    }

    return taskList;
}

void ContentManager::addVirtualItem(const std::shared_ptr<CdsObject>& obj, bool allow_fifo)
{
    obj->validate();
    fs::path path = obj->getLocation();

    std::error_code ec;
    auto dirEnt = fs::directory_entry(path, ec);
    if (ec || !dirEnt.is_regular_file(ec))
        throw_std_runtime_error("Not a file: {} - {}", path.c_str(), ec.message());

    auto pcdir = database->findObjectByPath(path);
    if (pcdir == nullptr) {
        pcdir = createObjectFromFile(dirEnt, true, allow_fifo);
        if (pcdir == nullptr) {
            throw_std_runtime_error("Could not add {}", path.c_str());
        }
        if (pcdir->isItem()) {
            this->addObject(pcdir, true);
            obj->setRefID(pcdir->getID());
        }
    }

    addObject(obj, true);
}

template <typename TP>
std::time_t to_time_t(TP tp)
{
    auto asSystemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(tp - TP::clock::now()
        + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(asSystemTime);
}

std::shared_ptr<CdsObject> ContentManager::createSingleItem(const fs::directory_entry& dirEnt, fs::path& rootPath, bool followSymlinks, bool checkDatabase, bool processExisting, bool firstChild, const std::shared_ptr<CMAddFileTask>& task)
{
    auto obj = checkDatabase ? database->findObjectByPath(dirEnt.path()) : nullptr;
    bool isNew = false;

    if (obj == nullptr) {
        obj = createObjectFromFile(dirEnt, followSymlinks);
        if (obj == nullptr) { // object ignored
            log_debug("Link to file or directory ignored: {}", dirEnt.path().c_str());
            return nullptr;
        }
        if (obj->isItem()) {
            addObject(obj, firstChild);
            isNew = true;
        }
    } else if (obj->isItem() && processExisting) {
        MetadataHandler::setMetadata(context, std::static_pointer_cast<CdsItem>(obj), dirEnt);
    }
    if (obj->isItem() && layout != nullptr && (processExisting || isNew)) {
        try {
            if (rootPath.empty() && (task != nullptr))
                rootPath = task->getRootPath();

            layout->processCdsObject(obj, rootPath);

            std::string mimetype = std::static_pointer_cast<CdsItem>(obj)->getMimeType();
            std::string content_type = getValueOrDefault(mimetype_contenttype_map, mimetype);

#ifdef HAVE_JS
            if ((playlist_parser_script != nullptr) && (content_type == CONTENT_TYPE_PLAYLIST))
                playlist_parser_script->processPlaylistObject(obj, task);
#else
            if (content_type == CONTENT_TYPE_PLAYLIST)
                log_warning("Playlist {} will not be parsed: Gerbera was compiled without JS support!", obj->getLocation().c_str());
#endif // JS
        } catch (const std::runtime_error& e) {
            log_error("{}", e.what());
        }
    }
    return obj;
}

int ContentManager::_addFile(const fs::directory_entry& dirEnt, fs::path rootPath, AutoScanSetting& asSetting, const std::shared_ptr<CMAddFileTask>& task)
{
    if (!asSetting.hidden) {
        if (dirEnt.path().is_relative())
            return INVALID_OBJECT_ID;
    }

    // never add the server configuration file
    if (config->getConfigFilename() == dirEnt.path())
        return INVALID_OBJECT_ID;

    // checkDatabase, don't process existing
    auto obj = createSingleItem(dirEnt, rootPath, asSetting.followSymlinks, true, false, false, task);
    if (obj == nullptr) // object ignored
        return INVALID_OBJECT_ID;

    if (asSetting.recursive && obj->isContainer()) {
        addRecursive(asSetting.adir, dirEnt, asSetting.followSymlinks, asSetting.hidden, task);
    }

    if (asSetting.rescanResource && obj->hasResource(CH_RESOURCE)) {
        auto parentPath = dirEnt.path().parent_path().string();
        updateAttachedResources(asSetting.adir, obj->getLocation().c_str(), parentPath, true);
    }

    return obj->getID();
}

bool ContentManager::updateAttachedResources(const std::shared_ptr<AutoscanDirectory>& adir, const char* location, const std::string& parentPath, bool all)
{
    bool parentRemoved = false;
    int parentID = database->findObjectIDByPath(parentPath, false);
    if (parentID != INVALID_OBJECT_ID) {
        // as there is no proper way to force refresh of unchanged files, delete whole dir and rescan it
        _removeObject(adir, parentID, false, all);
        // in order to rescan whole directory we have to set lmt to a very small value
        AutoScanSetting asSetting;
        asSetting.adir = adir;
        adir->setCurrentLMT(parentPath, time_t(1));
        asSetting.followSymlinks = config->getBoolOption(CFG_IMPORT_FOLLOW_SYMLINKS);
        asSetting.hidden = config->getBoolOption(CFG_IMPORT_HIDDEN_FILES);
        asSetting.recursive = true;
        asSetting.rescanResource = false;
        asSetting.mergeOptions(config, parentPath);
        std::error_code ec;
        // addFile(const fs::directory_entry& path, AutoScanSetting& asSetting, bool async, bool lowPriority, bool cancellable)
        auto dirEntry = fs::directory_entry(parentPath, ec);
        if (!ec) {
            addFile(dirEntry, asSetting, true, true, false);
            log_debug("Forced rescan of {} for resource {}", parentPath.c_str(), location);
            parentRemoved = true;
        } else {
            log_error("Failed to read {}: {}", parentPath.c_str(), ec.message());
        }
    }
    return parentRemoved;
}

void ContentManager::_removeObject(const std::shared_ptr<AutoscanDirectory>& adir, int objectID, bool rescanResource, bool all)
{
    if (objectID == CDS_ID_ROOT)
        throw_std_runtime_error("cannot remove root container");
    if (objectID == CDS_ID_FS_ROOT)
        throw_std_runtime_error("cannot remove PC-Directory container");
    if (IS_FORBIDDEN_CDS_ID(objectID))
        throw_std_runtime_error("tried to remove illegal object id");

    bool parentRemoved = false;
    if (rescanResource) {
        auto obj = database->loadObject(objectID);
        if (obj != nullptr && obj->hasResource(CH_RESOURCE)) {
            auto parentPath = obj->getLocation().parent_path().string();
            parentRemoved = updateAttachedResources(adir, obj->getLocation().c_str(), parentPath, all);
        }
    }
    // Removing a file can lead to virtual directories to drop empty and be removed
    // So current container cache must be invalidated
    containerMap.clear();

    if (!parentRemoved) {
        auto changedContainers = database->removeObject(objectID, all);
        if (changedContainers != nullptr) {
            session_manager->containerChangedUI(changedContainers->ui);
            update_manager->containersChanged(changedContainers->upnp);
        }
    }
    // reload accounting
    // loadAccounting();
}

int ContentManager::ensurePathExistence(fs::path path)
{
    int updateID;
    int containerID = database->ensurePathExistence(std::move(path), &updateID);
    if (updateID != INVALID_OBJECT_ID) {
        update_manager->containerChanged(updateID);
        session_manager->containerChangedUI(updateID);
    }
    return containerID;
}

void ContentManager::_rescanDirectory(std::shared_ptr<AutoscanDirectory>& adir, int containerID, const std::shared_ptr<GenericTask>& task)
{
    log_debug("start");

    if (adir == nullptr)
        throw_std_runtime_error("ID valid but nullptr returned? this should never happen");

    fs::path rootpath = adir->getLocation();

    fs::path location;
    std::shared_ptr<CdsContainer> parentContainer;

    if (containerID != INVALID_OBJECT_ID) {
        try {
            std::shared_ptr<CdsObject> obj = database->loadObject(containerID);
            if (!obj || !obj->isContainer()) {
                throw_std_runtime_error("Item {} is not a container", containerID);
            }
            location = (containerID == CDS_ID_FS_ROOT) ? FS_ROOT_DIRECTORY : obj->getLocation();
            parentContainer = std::dynamic_pointer_cast<CdsContainer>(obj);
        } catch (const std::runtime_error& e) {
            if (adir->persistent()) {
                containerID = INVALID_OBJECT_ID;
            } else {
                removeAutoscanDirectory(adir);
                return;
            }
        }
    }

    if (containerID == INVALID_OBJECT_ID) {
        if (!fs::is_directory(adir->getLocation())) {
            adir->setObjectID(INVALID_OBJECT_ID);
            database->updateAutoscanDirectory(adir);
            if (adir->persistent()) {
                return;
            }

            removeAutoscanDirectory(adir);
            return;
        }

        containerID = ensurePathExistence(adir->getLocation());
        adir->setObjectID(containerID);
        database->updateAutoscanDirectory(adir);
        location = adir->getLocation();
    }

    if (location.empty()) {
        log_error("Container with ID {} has no location information", containerID);
        return;
    }

    log_debug("Rescanning location: {}", location.c_str());

    std::error_code ec;
    auto rootDir = fs::directory_entry(location, ec);
    fs::directory_iterator dIter;

    if (!ec && rootDir.exists(ec) && rootDir.is_directory(ec)) {
        dIter = fs::directory_iterator(location, ec);
        if (ec) {
            log_error("_rescanDirectory: Failed to iterate {}, {}", location.c_str(), ec.message());
        }
    } else {
        log_error("Could not open {}: {}", location.c_str(), ec.message());
    }
    if (ec) {
        if (adir->persistent()) {
            removeObject(adir, containerID, false);
            if (location == adir->getLocation()) {
                adir->setObjectID(INVALID_OBJECT_ID);
                database->updateAutoscanDirectory(adir);
            }
            return;
        }

        if (location == adir->getLocation()) {
            removeObject(adir, containerID, false);
            removeAutoscanDirectory(adir);
        }
        return;
    }

    AutoScanSetting asSetting;
    asSetting.adir = adir;
    asSetting.recursive = adir->getRecursive();
    asSetting.followSymlinks = config->getBoolOption(CFG_IMPORT_FOLLOW_SYMLINKS);
    asSetting.hidden = adir->getHidden();
    asSetting.mergeOptions(config, location);

    log_debug("Rescanning options {}: recursive={} hidden={} followSymlinks={}", location.c_str(), asSetting.recursive, asSetting.hidden, asSetting.followSymlinks);

    // request only items if non-recursive scan is wanted
    auto list = database->getObjects(containerID, !asSetting.recursive);

    unsigned int thisTaskID;
    if (task != nullptr) {
        thisTaskID = task->getID();
    } else {
        thisTaskID = 0;
    }

    time_t last_modified_current_max = adir->getPreviousLMT(location, parentContainer);
    time_t last_modified_new_max = last_modified_current_max;
    adir->setCurrentLMT(location, 0);

    for (const auto& dirEnt : dIter) {
        const auto& newPath = dirEnt.path();
        const auto& name = newPath.filename().string();
        if (name[0] == '.' && !asSetting.hidden) {
            continue;
        }

        if ((shutdownFlag) || ((task != nullptr) && !task->isValid()))
            break;

        // it is possible that someone hits remove while the container is being scanned
        // in this case we will invalidate the autoscan entry
        if (adir->getScanID() == INVALID_SCAN_ID) {
            log_info("lost autoscan for {}", newPath.c_str());
            finishScan(adir, location, parentContainer, last_modified_new_max);
            return;
        }

        if (!asSetting.followSymlinks && dirEnt.is_symlink()) {
            int objectID = database->findObjectIDByPath(newPath);
            if (objectID > 0) {
                if (list != nullptr)
                    list->erase(objectID);
                removeObject(adir, objectID, false);
            }
            log_debug("link {} skipped", newPath.c_str());
            continue;
        }

        asSetting.recursive = adir->getRecursive();
        asSetting.followSymlinks = config->getBoolOption(CFG_IMPORT_FOLLOW_SYMLINKS);
        asSetting.hidden = adir->getHidden();
        asSetting.mergeOptions(config, location);
        auto lwt = to_time_t(dirEnt.last_write_time(ec));

        if (dirEnt.is_regular_file(ec)) {
            int objectID = database->findObjectIDByPath(newPath);
            if (objectID > 0) {
                if (list != nullptr)
                    list->erase(objectID);

                // check modification time and update file if chagned
                if (last_modified_current_max < lwt) {
                    // re-add object - we have to do this in order to trigger
                    // layout
                    removeObject(adir, objectID, false, false);
                    asSetting.recursive = false;
                    asSetting.rescanResource = false;
                    addFileInternal(dirEnt, rootpath, asSetting, false);
                    // update time variable
                    if (last_modified_new_max < lwt)
                        last_modified_new_max = lwt;
                }
            } else {
                // add file, not recursive, not async, not forced
                asSetting.recursive = false;
                asSetting.rescanResource = false;
                addFileInternal(dirEnt, rootpath, asSetting, false);
                if (last_modified_new_max < lwt)
                    last_modified_new_max = lwt;
            }
        } else if (dirEnt.is_directory(ec) && asSetting.recursive) {
            int objectID = database->findObjectIDByPath(newPath);
            if (last_modified_new_max < lwt)
                last_modified_new_max = lwt;
            if (objectID > 0) {
                log_debug("rescanSubDirectory {}", newPath.c_str());
                if (list != nullptr)
                    list->erase(objectID);
                // add a task to rescan the directory that was found
                rescanDirectory(adir, objectID, newPath, task->isCancellable());
            } else {
                log_debug("addSubDirectory {}", newPath.c_str());

                // we have to make sure that we will never add a path to the task list
                // if it is going to be removed by a pending remove task.
                // this lock will make sure that remove is not in the process of invalidating
                // the AutocsanDirectories in the autoscan_timed list at the time when we
                // are checking for validity.
                auto lock = threadRunner->lockGuard("addSubDirectory " + newPath.string());

                // it is possible that someone hits remove while the container is being scanned
                // in this case we will invalidate the autoscan entry
                if (adir->getScanID() == INVALID_SCAN_ID) {
                    log_info("lost autoscan for {}", newPath.c_str());
                    finishScan(adir, location, parentContainer, last_modified_new_max);
                    return;
                }
                // add directory, recursive, async, hidden flag, low priority
                asSetting.recursive = true;
                asSetting.rescanResource = false;
                asSetting.mergeOptions(config, newPath);
                // const fs::path& path, const fs::path& rootpath, AutoScanSetting& asSetting, bool async, bool lowPriority, unsigned int parentTaskID, bool cancellable
                addFileInternal(dirEnt, rootpath, asSetting, true, true, thisTaskID, task->isCancellable());
                log_debug("addSubDirectory {} done", newPath.c_str());
            }
        }
        if (ec) {
            log_error("_rescanDirectory: Failed to read {}, {}", newPath.c_str(), ec.message());
        }
    } // while

    finishScan(adir, location, parentContainer, last_modified_new_max);

    if ((shutdownFlag) || ((task != nullptr) && !task->isValid())) {
        return;
    }
    if (list != nullptr && !list->empty()) {
        auto changedContainers = database->removeObjects(list);
        if (changedContainers != nullptr) {
            session_manager->containerChangedUI(changedContainers->ui);
            update_manager->containersChanged(changedContainers->upnp);
        }
    }
}

/* scans the given directory and adds everything recursively */
void ContentManager::addRecursive(std::shared_ptr<AutoscanDirectory>& adir, const fs::directory_entry& subDir, bool followSymlinks, bool hidden, const std::shared_ptr<CMAddFileTask>& task)
{
    auto f2i = StringConverter::f2i(config);

    std::error_code ec;
    if (!subDir.exists(ec) || !subDir.is_directory(ec)) {
        throw_std_runtime_error("Could not list directory {}: {}", subDir.path().c_str(), ec.message());
    }

    int parentID = database->findObjectIDByPath(subDir.path());
    std::shared_ptr<CdsContainer> parentContainer;

    if (parentID != INVALID_OBJECT_ID) {
        try {
            std::shared_ptr<CdsObject> obj = database->loadObject(parentID);
            if (!obj || !obj->isContainer()) {
                throw_std_runtime_error("Item {} is not a container", parentID);
            }
            parentContainer = std::dynamic_pointer_cast<CdsContainer>(obj);
        } catch (const std::runtime_error& e) {
        }
    }

    // abort loop if either:
    // no valid directory returned, server is about to shutdown, the task is there and was invalidated
    if (task != nullptr) {
        log_debug("IS TASK VALID? [{}], task path: [{}]", task->isValid(), subDir.path().c_str());
    }
#ifdef HAVE_INOTIFY
    if (adir == nullptr) {
        for (size_t i = 0; i < autoscan_inotify->size(); i++) {
            log_debug("AutoDir {}", i);
            std::shared_ptr<AutoscanDirectory> dir = autoscan_inotify->get(i);
            if (dir != nullptr && startswith(dir->getLocation(), subDir.path()) && fs::is_directory(dir->getLocation())) {
                adir = dir;
            }
        }
    }
#endif
    time_t last_modified_current_max = 0;
    time_t last_modified_new_max = last_modified_current_max;
    if (adir != nullptr) {
        last_modified_current_max = adir->getPreviousLMT(subDir.path(), parentContainer);
        last_modified_new_max = last_modified_current_max;
        adir->setCurrentLMT(subDir.path(), 0);
    }
    auto dIter = fs::directory_iterator(subDir, ec);
    if (ec) {
        log_error("addRecursive: Failed to iterate {}, {}", subDir.path().c_str(), ec.message());
        return;
    }

    bool firstChild = true;
    for (const auto& subDirEnt : dIter) {
        const auto& newPath = subDirEnt.path();
        const auto& name = newPath.filename().string();
        if (name[0] == '.' && !hidden) {
            continue;
        }
        if ((shutdownFlag) || ((task != nullptr) && !task->isValid()))
            break;

        if (config->getConfigFilename() == newPath)
            continue;

        // For the Web UI
        if (task != nullptr) {
            task->setDescription(fmt::format("Importing: {}", newPath.c_str()));
        }

        try {
            fs::path rootPath("");
            // check database if parent, process existing
            auto obj = createSingleItem(subDirEnt, rootPath, followSymlinks, (parentID > 0), true, firstChild, task);

            if (obj != nullptr) {
                firstChild = false;
                auto lwt = to_time_t(subDirEnt.last_write_time(ec));
                if (last_modified_current_max < lwt) {
                    last_modified_new_max = lwt;
                }
                if (obj->isItem()) {
                    parentID = obj->getParentID();
                }
                if (obj->isContainer()) {
                    addRecursive(adir, subDirEnt, followSymlinks, hidden, task);
                }
            }
        } catch (const std::runtime_error& ex) {
            log_warning("skipping {} (ex:{})", newPath.c_str(), ex.what());
        }
    }

    finishScan(adir, subDir.path(), parentContainer, last_modified_new_max);
}

void ContentManager::finishScan(const std::shared_ptr<AutoscanDirectory>& adir, const std::string& location, std::shared_ptr<CdsContainer>& parent, time_t lmt)
{
    if (adir != nullptr) {
        adir->setCurrentLMT(location, lmt > 0 ? lmt : (time_t)1);
        if (parent && lmt > 0) {
            parent->setMTime(lmt);
            int changedContainer;
            database->updateObject(parent, &changedContainer);
        }
    }
}

template <typename T>
void ContentManager::updateCdsObject(std::shared_ptr<T>& item, const std::map<std::string, std::string>& parameters)
{
    std::string title = getValueOrDefault(parameters, "title");
    std::string upnp_class = getValueOrDefault(parameters, "class");
    std::string autoscan = getValueOrDefault(parameters, "autoscan");
    std::string mimetype = getValueOrDefault(parameters, "mime-type");
    std::string description = getValueOrDefault(parameters, "description");
    std::string location = getValueOrDefault(parameters, "location");
    std::string protocol = getValueOrDefault(parameters, "protocol");
    std::string bookmarkpos = getValueOrDefault(parameters, "bookmarkpos");

    log_error("updateCdsObject: CdsObject {} not updated", title);
}

template <>
void ContentManager::updateCdsObject(std::shared_ptr<CdsContainer>& item, const std::map<std::string, std::string>& parameters)
{
    std::string title = getValueOrDefault(parameters, "title");
    std::string upnp_class = getValueOrDefault(parameters, "class");

    log_debug("updateCdsObject: CdsContainer {} updated", title);

    auto clone = CdsObject::createObject(item->getObjectType());
    item->copyTo(clone);

    if (!title.empty())
        clone->setTitle(title);
    if (!upnp_class.empty())
        clone->setClass(upnp_class);

    auto cloned_item = std::static_pointer_cast<CdsContainer>(clone);

    if (!item->equals(cloned_item, true)) {
        clone->validate();
        int containerChanged = INVALID_OBJECT_ID;
        database->updateObject(clone, &containerChanged);
        update_manager->containerChanged(containerChanged);
        session_manager->containerChangedUI(containerChanged);
        update_manager->containerChanged(item->getParentID());
        session_manager->containerChangedUI(item->getParentID());
    }
}

template <>
void ContentManager::updateCdsObject(std::shared_ptr<CdsItem>& item, const std::map<std::string, std::string>& parameters)
{
    std::string title = getValueOrDefault(parameters, "title");
    std::string upnp_class = getValueOrDefault(parameters, "class");
    std::string mimetype = getValueOrDefault(parameters, "mime-type");
    std::string description = getValueOrDefault(parameters, "description");
    std::string location = getValueOrDefault(parameters, "location");
    std::string protocol = getValueOrDefault(parameters, "protocol");
    std::string bookmarkpos = getValueOrDefault(parameters, "bookmarkpos");

    log_debug("updateCdsObject: CdsItem {} updated", title);

    auto clone = CdsObject::createObject(item->getObjectType());
    item->copyTo(clone);

    if (!title.empty())
        clone->setTitle(title);
    if (!upnp_class.empty())
        clone->setClass(upnp_class);
    if (!location.empty())
        clone->setLocation(location);

    auto cloned_item = std::static_pointer_cast<CdsItem>(clone);

    if (!bookmarkpos.empty())
        cloned_item->setBookMarkPos(stoiString(bookmarkpos));
    if (!mimetype.empty() && !protocol.empty()) {
        cloned_item->setMimeType(mimetype);
        auto resource = cloned_item->getResource(0);
        resource->addAttribute(R_PROTOCOLINFO, renderProtocolInfo(mimetype, protocol));
    } else if (mimetype.empty() && !protocol.empty()) {
        auto resource = cloned_item->getResource(0);
        resource->addAttribute(R_PROTOCOLINFO, renderProtocolInfo(cloned_item->getMimeType(), protocol));
    } else if (!mimetype.empty()) {
        cloned_item->setMimeType(mimetype);
        auto resource = cloned_item->getResource(0);
        std::vector<std::string> parts = splitString(resource->getAttribute(R_PROTOCOLINFO), ':');
        protocol = parts[0];
        resource->addAttribute(R_PROTOCOLINFO, renderProtocolInfo(mimetype, protocol));
    }

    if (!description.empty()) {
        cloned_item->setMetadata(M_DESCRIPTION, description);
    } else {
        cloned_item->removeMetadata(M_DESCRIPTION);
    }

    log_debug("updateCdsObject: checking equality of item {}", item->getTitle().c_str());
    if (!item->equals(cloned_item, true)) {
        cloned_item->validate();
        int containerChanged = INVALID_OBJECT_ID;
        database->updateObject(clone, &containerChanged);
        update_manager->containerChanged(containerChanged);
        session_manager->containerChangedUI(containerChanged);
        log_debug("updateObject: calling containerChanged on item {}", item->getTitle().c_str());
        update_manager->containerChanged(item->getParentID());
    }
}

void ContentManager::updateObject(int objectID, const std::map<std::string, std::string>& parameters)
{
    auto obj = database->loadObject(objectID);
    auto item = std::dynamic_pointer_cast<CdsItem>(obj);
    if (item != nullptr) {
        updateCdsObject(item, parameters);
    } else {
        auto cont = std::dynamic_pointer_cast<CdsContainer>(obj);
        if (cont != nullptr) {
            updateCdsObject(cont, parameters);
        } else {
            updateCdsObject(obj, parameters);
        }
    }
}

void ContentManager::addObject(const std::shared_ptr<CdsObject>& obj, bool firstChild)
{
    obj->validate();

    int containerChanged = INVALID_OBJECT_ID;
    log_debug("Adding: parent ID is {}", obj->getParentID());

    database->addObject(obj, &containerChanged);
    log_debug("After adding: parent ID is {}", obj->getParentID());

    update_manager->containerChanged(containerChanged);
    session_manager->containerChangedUI(containerChanged);

    int parent_id = obj->getParentID();
    // this is the first entry, so the container is new also, send update for parent of parent
    if (firstChild) {
        firstChild = (database->getChildCount(parent_id) == 1);
    }
    if ((parent_id != -1) && firstChild) {
        auto parent = database->loadObject(parent_id);
        log_debug("Will update parent ID {}", parent->getParentID());
        update_manager->containerChanged(parent->getParentID());
    }

    update_manager->containerChanged(obj->getParentID());
    if (obj->isContainer())
        session_manager->containerChangedUI(obj->getParentID());
}

void ContentManager::addContainer(int parentID, std::string title, const std::string& upnpClass)
{
    addContainerChain(database->buildContainerPath(parentID, escape(std::move(title), VIRTUAL_CONTAINER_ESCAPE, VIRTUAL_CONTAINER_SEPARATOR)), upnpClass);
}

std::pair<int, bool> ContentManager::addContainerTree(const std::vector<std::shared_ptr<CdsObject>>& chain)
{
    std::string tree;
    int result = INVALID_OBJECT_ID;
    std::vector<int> createdIds;
    bool isNew = false;

    for (const auto& item : chain) {
        if (item->getTitle().empty()) {
            log_error("Received chain item without title");
            return { INVALID_OBJECT_ID, false };
        }
        tree = fmt::format("{}{}{}", tree, VIRTUAL_CONTAINER_SEPARATOR, item->getTitle());
        log_debug("Received chain item {}", tree);
        for (const auto& [key, val] : config->getDictionaryOption(CFG_IMPORT_LAYOUT_MAPPING)) {
            tree = std::regex_replace(tree, std::regex(key), val);
        }
        if (!containerMap.count(tree)) {
            item->setMetadata(M_TITLE, item->getTitle());
            database->addContainerChain(tree, item->getClass(), INVALID_OBJECT_ID, &result, createdIds, item->getMetadata());
            auto container = std::dynamic_pointer_cast<CdsContainer>(database->loadObject(result));
            containerMap[tree] = container;
            isNew = true;
        } else {
            result = containerMap[tree]->getID();
        }
        assignFanArt({ containerMap[tree] }, item);
    }

    if (!createdIds.empty()) {
        update_manager->containerChanged(result);
        session_manager->containerChangedUI(result);
    }
    return { result, isNew };
}

std::pair<int, bool> ContentManager::addContainerChain(const std::string& chain, const std::string& lastClass, int lastRefID, const std::shared_ptr<CdsObject>& origObj)
{
    std::map<std::string, std::string> lastMetadata = origObj != nullptr ? origObj->getMetadata() : std::map<std::string, std::string>();
    std::vector<int> updateID;
    bool isNew = false;

    if (chain.empty())
        throw_std_runtime_error("addContainerChain() called with empty chain parameter");

    std::string newChain = chain;
    for (const auto& [key, val] : config->getDictionaryOption(CFG_IMPORT_LAYOUT_MAPPING)) {
        newChain = std::regex_replace(newChain, std::regex(key), val);
    }

    log_debug("Received chain: {} -> {} ({}) [{}]", chain.c_str(), newChain.c_str(), lastClass.c_str(), dictEncodeSimple(lastMetadata).c_str());
    // copy artist to album artist if empty
    const auto aaItm = lastMetadata.find(MetadataHandler::getMetaFieldName(M_ALBUMARTIST));
    const auto taItm = lastMetadata.find(MetadataHandler::getMetaFieldName(M_ARTIST));
    if (aaItm == lastMetadata.end() && taItm != lastMetadata.end()) {
        lastMetadata[MetadataHandler::getMetaFieldName(M_ALBUMARTIST)] = taItm->second;
    }

    constexpr auto unwanted = std::array { M_DESCRIPTION, M_TITLE, M_TRACKNUMBER, M_ARTIST }; // not wanted for container!
    for (const auto& unw : unwanted) {
        const auto itm = lastMetadata.find(MetadataHandler::getMetaFieldName(unw));
        if (itm != lastMetadata.end()) {
            lastMetadata.erase(itm);
        }
    }
    int containerID = INVALID_OBJECT_ID;
    std::vector<std::shared_ptr<CdsContainer>> containerList;
    if (!containerMap.count(newChain)) {
        lastMetadata[MetadataHandler::getMetaFieldName(M_TITLE)] = splitString(newChain, '/').back();
        database->addContainerChain(newChain, lastClass, lastRefID, &containerID, updateID, lastMetadata);

        for (const auto& contId : updateID) {
            auto container = std::dynamic_pointer_cast<CdsContainer>(database->loadObject(contId));
            containerMap[container->getLocation()] = container;
            containerList.emplace_back(container);
        }
        isNew = true;
    } else {
        containerID = containerMap[newChain]->getID();
        containerList.emplace_back(containerMap[newChain]);
    }

    if (!updateID.empty()) {
        assignFanArt(containerList, origObj);
        update_manager->containerChanged(updateID.back());
        session_manager->containerChangedUI(updateID.back());
    }

    return { containerID, isNew };
}

void ContentManager::assignFanArt(const std::vector<std::shared_ptr<CdsContainer>>& containerList, const std::shared_ptr<CdsObject>& origObj)
{
    if (origObj != nullptr) {
        int count = 0;
        for (auto& container : containerList) {
            const std::vector<std::shared_ptr<CdsResource>>& resources = container->getResources();
            auto fanart = std::find_if(resources.begin(), resources.end(), [=](const auto& res) { return res->isMetaResource(ID3_ALBUM_ART); });
            if (fanart == resources.end()) {
                MetadataHandler::createHandler(context, CH_CONTAINERART)->fillMetadata(container);
                int containerChanged = INVALID_OBJECT_ID;
                database->updateObject(container, &containerChanged);
                fanart = std::find_if(resources.begin(), resources.end(), [=](const auto& res) { return res->isMetaResource(ID3_ALBUM_ART); });
            }
            auto location = container->getLocation().string();
            if (fanart != resources.end() && (*fanart)->getHandlerType() != CH_CONTAINERART) {
                // remove stale references
                auto fanartObjId = stoiString((*fanart)->getAttribute(R_FANART_OBJ_ID));
                try {
                    if (fanartObjId > 0) {
                        database->loadObject(fanartObjId);
                    }
                } catch (const ObjectNotFoundException& e) {
                    container->removeResource((*fanart)->getHandlerType());
                    fanart = resources.end();
                }
            }
            if (fanart == resources.end() && (origObj->isContainer() || (count < config->getIntOption(CFG_IMPORT_RESOURCES_CONTAINERART_PARENTCOUNT) && container->getParentID() != CDS_ID_ROOT && std::count(location.begin(), location.end(), '/') > config->getIntOption(CFG_IMPORT_RESOURCES_CONTAINERART_MINDEPTH)))) {
                const std::vector<std::shared_ptr<CdsResource>>& origResources = origObj->getResources();
                fanart = std::find_if(origResources.begin(), origResources.end(), [=](const auto& res) { return res->isMetaResource(ID3_ALBUM_ART); });
                if (fanart != origResources.end()) {
                    if ((*fanart)->getAttribute(R_RESOURCE_FILE).empty()) {
                        (*fanart)->addAttribute(R_FANART_OBJ_ID, fmt::to_string(origObj->getID() != INVALID_OBJECT_ID ? origObj->getID() : origObj->getRefID()));
                        (*fanart)->addAttribute(R_FANART_RES_ID, fmt::to_string(fanart - origResources.begin()));
                    }
                    container->addResource(*fanart);
                }
                int containerChanged = INVALID_OBJECT_ID;
                database->updateObject(container, &containerChanged);
            }
            count++;
        }
    }
}

void ContentManager::updateObject(const std::shared_ptr<CdsObject>& obj, bool send_updates)
{
    obj->validate();

    int containerChanged = INVALID_OBJECT_ID;
    database->updateObject(obj, &containerChanged);

    if (send_updates) {
        update_manager->containerChanged(containerChanged);
        session_manager->containerChangedUI(containerChanged);

        update_manager->containerChanged(obj->getParentID());
        if (obj->isContainer())
            session_manager->containerChangedUI(obj->getParentID());
    }
}

std::shared_ptr<CdsObject> ContentManager::createObjectFromFile(const fs::directory_entry& dirEnt, bool followSymlinks, bool allow_fifo)
{
    std::error_code ec;

    if (!dirEnt.exists(ec)) {
        log_warning("File or directory does not exist: {} ({})", dirEnt.path().c_str(), ec.message());
        return nullptr;
    }

    if (!followSymlinks && dirEnt.is_symlink())
        return nullptr;

    std::shared_ptr<CdsObject> obj;
    if (dirEnt.is_regular_file(ec) || (allow_fifo && dirEnt.is_fifo(ec))) { // item
        /* retrieve information about item and decide if it should be included */
        std::string mimetype = mime->getMimeType(dirEnt.path(), MIMETYPE_DEFAULT);
        if (mimetype.empty()) {
            return nullptr;
        }
        log_debug("Mime '{}' for file {}", mimetype, dirEnt.path().c_str());

        std::string upnp_class = mime->mimeTypeToUpnpClass(mimetype);
        if (upnp_class.empty()) {
            std::string content_type = getValueOrDefault(mimetype_contenttype_map, mimetype);
            if (content_type == CONTENT_TYPE_OGG) {
                upnp_class = isTheora(dirEnt.path())
                    ? UPNP_CLASS_VIDEO_ITEM
                    : UPNP_CLASS_MUSIC_TRACK;
            }
        }
        log_debug("UpnpClass '{}' for file {}", upnp_class, dirEnt.path().c_str());

        auto item = std::make_shared<CdsItem>();
        obj = item;
        item->setLocation(dirEnt.path());
        item->setMTime(to_time_t(dirEnt.last_write_time(ec)));
        item->setSizeOnDisk(dirEnt.file_size());

        if (!mimetype.empty()) {
            item->setMimeType(mimetype);
        }
        if (!upnp_class.empty()) {
            item->setClass(upnp_class);
        }

        auto f2i = StringConverter::f2i(config);
        auto title = dirEnt.path().filename().string();
        if (config->getBoolOption(CFG_IMPORT_READABLE_NAMES) && upnp_class != UPNP_CLASS_ITEM) {
            title = dirEnt.path().stem().string();
            title = replaceAllString(title, "_", " ");
        }
        obj->setTitle(f2i->convert(title));

        MetadataHandler::setMetadata(context, item, dirEnt);
    } else if (dirEnt.is_directory(ec)) {
        auto cont = std::make_shared<CdsContainer>();
        obj = cont;
        /* adding containers is done by Database now
         * this exists only to inform the caller that
         * this is a container
         */
        /*
        cont->setMTime(to_time_t(dirEnt.last_write_time(ec)));
        cont->setLocation(path);
        auto f2i = StringConverter::f2i();
        obj->setTitle(f2i->convert(filename));
        */
    } else {
        // only regular files and directories are supported
        throw_std_runtime_error("ContentManager: skipping file {}", dirEnt.path().c_str());
    }
    if (ec) {
        log_error("File or directory cannot be read: {} ({})", dirEnt.path().c_str(), ec.message());
    }
    return obj;
}

void ContentManager::initLayout()
{
    if (layout == nullptr) {
        auto lock = threadRunner->lockGuard("initLayout");
        if (layout == nullptr) {
            std::string layout_type = config->getOption(CFG_IMPORT_SCRIPTING_VIRTUAL_LAYOUT_TYPE);
            auto self = shared_from_this();
            try {
                if (layout_type == "js") {
#ifdef HAVE_JS
                    layout = std::make_shared<JSLayout>(self, scripting_runtime);
#else
                    log_error("Cannot init layout: Gerbera compiled without JS support, but JS was requested.");
#endif
                } else if (layout_type == "builtin") {
                    layout = std::make_shared<BuiltinLayout>(self);
                }
            } catch (const std::runtime_error& e) {
                layout = nullptr;
                log_error("ContentManager virtual container layout: {}", e.what());
                if (layout_type != "disabled")
                    throw e;
            }
        }
    }
}

#ifdef HAVE_JS
void ContentManager::initJS()
{
    if (playlist_parser_script == nullptr) {
        auto self = shared_from_this();
        playlist_parser_script = std::make_unique<PlaylistParserScript>(self, scripting_runtime);
    }
}

void ContentManager::destroyJS() { playlist_parser_script = nullptr; }

#endif // HAVE_JS

void ContentManager::destroyLayout()
{
    layout = nullptr;
}

void ContentManager::reloadLayout()
{
    destroyLayout();
#ifdef HAVE_JS
    destroyJS();
#endif // HAVE_JS
    initLayout();
#ifdef HAVE_JS
    initJS();
#endif // HAVE_JS
}

void ContentManager::threadProc()
{
    std::shared_ptr<GenericTask> task;
    auto lock = threadRunner->uniqueLock();
    working = true;
    while (!shutdownFlag) {
        currentTask = nullptr;

        task = nullptr;
        if (!taskQueue1.empty()) {
            task = taskQueue1.front();
            taskQueue1.pop_front();
        } else if (!taskQueue2.empty()) {
            task = taskQueue2.front();
            taskQueue2.pop_front();
        }

        if (task == nullptr) {
            working = false;
            /* if nothing to do, sleep until awakened */
            threadRunner->wait(lock);
            working = true;
            continue;
        }

        currentTask = task;
        lock.unlock();

        // log_debug("content manager Async START {}", task->getDescription().c_str());
        try {
            if (task->isValid())
                task->run();
        } catch (const ServerShutdownException& se) {
            shutdownFlag = true;
        } catch (const std::runtime_error& e) {
            log_error("Exception caught: {}", e.what());
        }
        // log_debug("content manager ASYNC STOP  {}", task->getDescription().c_str());

        if (!shutdownFlag) {
            lock.lock();
        }
    }

    database->threadCleanup();
}

void* ContentManager::staticThreadProc(void* arg)
{
    auto inst = static_cast<ContentManager*>(arg);
    inst->threadProc();
    return nullptr;
}

void ContentManager::addTask(const std::shared_ptr<GenericTask>& task, bool lowPriority)
{
    auto lock = threadRunner->lockGuard("addTask");

    task->setID(taskID++);

    if (!lowPriority)
        taskQueue1.push_back(task);
    else
        taskQueue2.push_back(task);
    threadRunner->notify();
}

int ContentManager::addFile(const fs::directory_entry& dirEnt, AutoScanSetting& asSetting, bool async, bool lowPriority, bool cancellable)
{
    fs::path rootpath;
    if (dirEnt.is_directory())
        rootpath = dirEnt.path();
    return addFileInternal(dirEnt, rootpath, asSetting, async, lowPriority, 0, cancellable);
}

int ContentManager::addFile(const fs::directory_entry& dirEnt, const fs::path& rootpath, AutoScanSetting& asSetting, bool async, bool lowPriority, bool cancellable)
{
    return addFileInternal(dirEnt, rootpath, asSetting, async, lowPriority, 0, cancellable);
}

int ContentManager::addFileInternal(
    const fs::directory_entry& dirEnt, const fs::path& rootpath, AutoScanSetting& asSetting, bool async, bool lowPriority, unsigned int parentTaskID, bool cancellable)
{
    if (async) {
        auto self = shared_from_this();
        auto task = std::make_shared<CMAddFileTask>(self, dirEnt, rootpath, asSetting, cancellable);
        task->setDescription(fmt::format("Importing: {}", dirEnt.path().c_str()));
        task->setParentID(parentTaskID);
        addTask(task, lowPriority);
        return INVALID_OBJECT_ID;
    }
    return _addFile(dirEnt, rootpath, asSetting);
}

#ifdef ONLINE_SERVICES
void ContentManager::fetchOnlineContent(service_type_t serviceType, bool lowPriority, bool cancellable, bool unscheduled_refresh)
{
    auto service = online_services->getService(serviceType);
    if (service == nullptr) {
        log_debug("No surch service! {}", serviceType);
        throw_std_runtime_error("Service not found");
    }

    unsigned int parentTaskID = 0;

    auto self = shared_from_this();
    auto task = std::make_shared<CMFetchOnlineContentTask>(self, task_processor, timer, service, layout, cancellable, unscheduled_refresh);
    task->setDescription("Updating content from " + service->getServiceName());
    task->setParentID(parentTaskID);
    service->incTaskCount();
    addTask(task, lowPriority);
}

void ContentManager::cleanupOnlineServiceObjects(const std::shared_ptr<OnlineService>& service)
{
    log_debug("Finished fetch cycle for service: {}", service->getServiceName().c_str());

    if (service->getItemPurgeInterval() > 0) {
        auto ids = database->getServiceObjectIDs(service->getDatabasePrefix());

        auto current = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        int64_t last = 0;
        std::string temp;

        for (int object_id : *ids) {
            auto obj = database->loadObject(object_id);
            if (obj == nullptr)
                continue;

            temp = obj->getAuxData(ONLINE_SERVICE_LAST_UPDATE);
            if (temp.empty())
                continue;

            last = std::stoll(temp);

            if ((service->getItemPurgeInterval() > 0) && ((current - last) > service->getItemPurgeInterval())) {
                log_debug("Purging old online service object {}", obj->getTitle().c_str());
                removeObject(nullptr, object_id, false);
            }
        }
    }
}

#endif

void ContentManager::invalidateAddTask(const std::shared_ptr<GenericTask>& t, const fs::path& path)
{
    if (t->getType() == AddFile) {
        auto add_task = std::static_pointer_cast<CMAddFileTask>(t);
        log_debug("comparing, task path: {}, remove path: {}", add_task->getPath().c_str(), path.c_str());
        if (startswith(add_task->getPath(), path)) {
            log_debug("Invalidating task with path {}", add_task->getPath().c_str());
            add_task->invalidate();
        }
    }
}

void ContentManager::invalidateTask(unsigned int taskID, task_owner_t taskOwner)
{
    if (taskOwner == ContentManagerTask) {
        auto lock = threadRunner->lockGuard("invalidateTask");
        auto tc = getCurrentTask();
        if (tc != nullptr) {
            if ((tc->getID() == taskID) || (tc->getParentID() == taskID)) {
                tc->invalidate();
            }
        }

        for (const auto& t1 : taskQueue1) {
            if ((t1->getID() == taskID) || (t1->getParentID() == taskID)) {
                t1->invalidate();
            }
        }

        for (const auto& t2 : taskQueue2) {
            if ((t2->getID() == taskID) || (t2->getParentID() == taskID)) {
                t2->invalidate();
            }
        }
    }
#ifdef ONLINE_SERVICES
    else if (taskOwner == TaskProcessorTask)
        task_processor->invalidateTask(taskID);
#endif
}

void ContentManager::removeObject(const std::shared_ptr<AutoscanDirectory>& adir, int objectID, bool rescanResource, bool async, bool all)
{
    if (async) {
        /*
        // building container path for the description
        auto objectPath = database->getObjectPath(objectID);
        std::ostringstream desc;
        desc << "Removing ";
        // skip root container, start from 1
        for (size_t i = 1; i < objectPath->size(); i++)
            desc << '/' << objectPath->get(i)->getTitle();
        */
        auto self = shared_from_this();
        auto task = std::make_shared<CMRemoveObjectTask>(self, adir, objectID, rescanResource, all);
        fs::path path;
        std::shared_ptr<CdsObject> obj;

        try {
            obj = database->loadObject(objectID);
            path = obj->getLocation();
        } catch (const std::runtime_error& e) {
            log_debug("trying to remove an object ID which is no longer in the database! {}", objectID);
            return;
        }

        if (obj->isContainer()) {
            // make sure to remove possible child autoscan directories from the scanlist
            std::shared_ptr<AutoscanList> rm_list = autoscan_timed->removeIfSubdir(path);
            for (size_t i = 0; i < rm_list->size(); i++) {
                timer->removeTimerSubscriber(this, rm_list->get(i)->getTimerParameter(), true);
            }
#ifdef HAVE_INOTIFY
            if (config->getBoolOption(CFG_IMPORT_AUTOSCAN_USE_INOTIFY)) {
                rm_list = autoscan_inotify->removeIfSubdir(path);
                for (size_t i = 0; i < rm_list->size(); i++) {
                    std::shared_ptr<AutoscanDirectory> dir = rm_list->get(i);
                    inotify->unmonitor(dir);
                }
            }
#endif

            auto lock = threadRunner->lockGuard("removeObject " + path.string());

            // we have to make sure that a currently running autoscan task will not
            // launch add tasks for directories that anyway are going to be deleted
            for (const auto& t : taskQueue1) {
                invalidateAddTask(t, path);
            }

            for (const auto& t : taskQueue2) {
                invalidateAddTask(t, path);
            }

            auto t = getCurrentTask();
            if (t != nullptr) {
                invalidateAddTask(t, path);
            }
        }

        addTask(task);
    } else {
        _removeObject(adir, objectID, rescanResource, all);
    }
}

void ContentManager::rescanDirectory(const std::shared_ptr<AutoscanDirectory>& adir, int objectId, std::string descPath, bool cancellable)
{
    // building container path for the description
    auto self = shared_from_this();
    auto task = std::make_shared<CMRescanDirectoryTask>(self, adir, objectId, cancellable);

    adir->incTaskCount();

    if (descPath.empty())
        descPath = adir->getLocation();

    task->setDescription("Scan: " + descPath);
    addTask(task, true); // adding with low priority
}

std::shared_ptr<AutoscanDirectory> ContentManager::getAutoscanDirectory(int scanID, ScanMode scanMode) const
{
    if (scanMode == ScanMode::Timed) {
        return autoscan_timed->get(scanID);
    }

#if HAVE_INOTIFY
    if (scanMode == ScanMode::INotify) {
        return autoscan_inotify->get(scanID);
    }
#endif
    return nullptr;
}

std::shared_ptr<AutoscanDirectory> ContentManager::getAutoscanDirectory(int objectID)
{
    return database->getAutoscanDirectory(objectID);
}

std::shared_ptr<AutoscanDirectory> ContentManager::getAutoscanDirectory(const fs::path& location) const
{
    // \todo change this when more scanmodes become available
    std::shared_ptr<AutoscanDirectory> adir = autoscan_timed->get(location);
#if HAVE_INOTIFY
    if (adir == nullptr)
        adir = autoscan_inotify->get(location);
#endif
    return adir;
}

std::vector<std::shared_ptr<AutoscanDirectory>> ContentManager::getAutoscanDirectories() const
{
    auto all = autoscan_timed->getArrayCopy();

#if HAVE_INOTIFY
    auto ino = autoscan_inotify->getArrayCopy();
    std::copy(ino.begin(), ino.end(), std::back_inserter(all));
#endif
    return all;
}

void ContentManager::removeAutoscanDirectory(const std::shared_ptr<AutoscanDirectory>& adir)
{
    if (adir == nullptr)
        throw_std_runtime_error("can not remove autoscan directory - was not an autoscan");

    adir->setTaskCount(-1);

    if (adir->getScanMode() == ScanMode::Timed) {
        autoscan_timed->remove(adir->getScanID());
        database->removeAutoscanDirectory(adir);
        session_manager->containerChangedUI(adir->getObjectID());

        // if 3rd parameter is true: won't fail if scanID doesn't exist
        timer->removeTimerSubscriber(this, adir->getTimerParameter(), true);
    }
#ifdef HAVE_INOTIFY
    if (config->getBoolOption(CFG_IMPORT_AUTOSCAN_USE_INOTIFY)) {
        if (adir->getScanMode() == ScanMode::INotify) {
            autoscan_inotify->remove(adir->getScanID());
            database->removeAutoscanDirectory(adir);
            session_manager->containerChangedUI(adir->getObjectID());
            inotify->unmonitor(adir);
        }
    }
#endif
}

void ContentManager::handlePeristentAutoscanRemove(const std::shared_ptr<AutoscanDirectory>& adir)
{
    if (adir->persistent()) {
        adir->setObjectID(INVALID_OBJECT_ID);
        database->updateAutoscanDirectory(adir);
    } else {
        removeAutoscanDirectory(adir);
    }
}

void ContentManager::handlePersistentAutoscanRecreate(const std::shared_ptr<AutoscanDirectory>& adir)
{
    int id = ensurePathExistence(adir->getLocation());
    adir->setObjectID(id);
    database->updateAutoscanDirectory(adir);
}

void ContentManager::setAutoscanDirectory(const std::shared_ptr<AutoscanDirectory>& dir)
{
    std::shared_ptr<AutoscanDirectory> original;

    // We will have to change this for other scan modes
    original = autoscan_timed->getByObjectID(dir->getObjectID());
#ifdef HAVE_INOTIFY
    if (config->getBoolOption(CFG_IMPORT_AUTOSCAN_USE_INOTIFY)) {
        if (original == nullptr)
            original = autoscan_inotify->getByObjectID(dir->getObjectID());
    }
#endif

    if (original != nullptr)
        dir->setDatabaseID(original->getDatabaseID());

    database->checkOverlappingAutoscans(dir);

    // adding a new autoscan directory
    if (original == nullptr) {
        if (dir->getObjectID() == CDS_ID_FS_ROOT)
            dir->setLocation(FS_ROOT_DIRECTORY);
        else {
            log_debug("objectID: {}", dir->getObjectID());
            auto obj = database->loadObject(dir->getObjectID());
            if (obj == nullptr || !obj->isContainer() || obj->isVirtual())
                throw_std_runtime_error("tried to remove an illegal object (id) from the list of the autoscan directories");

            log_debug("location: {}", obj->getLocation().c_str());

            if (obj->getLocation().empty())
                throw_std_runtime_error("tried to add an illegal object as autoscan - no location information available");

            dir->setLocation(obj->getLocation());
        }
        dir->resetLMT();
        database->addAutoscanDirectory(dir);
        if (dir->getScanMode() == ScanMode::Timed) {
            autoscan_timed->add(dir);
            reloadLayout();
            timerNotify(dir->getTimerParameter());
        }
#ifdef HAVE_INOTIFY
        if (config->getBoolOption(CFG_IMPORT_AUTOSCAN_USE_INOTIFY)) {
            if (dir->getScanMode() == ScanMode::INotify) {
                autoscan_inotify->add(dir);
                reloadLayout();
                inotify->monitor(dir);
            }
        }
#endif
        session_manager->containerChangedUI(dir->getObjectID());
        return;
    }

    if (original->getScanMode() == ScanMode::Timed)
        timer->removeTimerSubscriber(this, original->getTimerParameter(), true);
#ifdef HAVE_INOTIFY
    if (config->getBoolOption(CFG_IMPORT_AUTOSCAN_USE_INOTIFY)) {
        if (original->getScanMode() == ScanMode::INotify) {
            inotify->unmonitor(original);
        }
    }
#endif

    auto copy = std::make_shared<AutoscanDirectory>();
    original->copyTo(copy);

    copy->setHidden(dir->getHidden());
    copy->setRecursive(dir->getRecursive());
    copy->setInterval(dir->getInterval());

    if (copy->getScanMode() == ScanMode::Timed) {
        autoscan_timed->remove(copy->getScanID());
    }
#ifdef HAVE_INOTIFY
    if (config->getBoolOption(CFG_IMPORT_AUTOSCAN_USE_INOTIFY)) {
        if (copy->getScanMode() == ScanMode::INotify) {
            autoscan_inotify->remove(copy->getScanID());
        }
    }
#endif

    copy->setScanMode(dir->getScanMode());

    if (dir->getScanMode() == ScanMode::Timed) {
        autoscan_timed->add(copy);
        timerNotify(copy->getTimerParameter());
    }
#ifdef HAVE_INOTIFY
    if (config->getBoolOption(CFG_IMPORT_AUTOSCAN_USE_INOTIFY)) {
        if (dir->getScanMode() == ScanMode::INotify) {
            autoscan_inotify->add(copy);
            inotify->monitor(copy);
        }
    }
#endif

    database->updateAutoscanDirectory(copy);
    if (original->getScanMode() != copy->getScanMode())
        session_manager->containerChangedUI(copy->getObjectID());
}

void ContentManager::triggerPlayHook(const std::shared_ptr<CdsObject>& obj)
{
    log_debug("start");

    if (config->getBoolOption(CFG_SERVER_EXTOPTS_MARK_PLAYED_ITEMS_ENABLED) && !obj->getFlag(OBJECT_FLAG_PLAYED)) {
        std::vector<std::string> mark_list = config->getArrayOption(CFG_SERVER_EXTOPTS_MARK_PLAYED_ITEMS_CONTENT_LIST);

        bool mark = std::any_of(mark_list.begin(), mark_list.end(), [&](const auto& i) { return startswith(std::static_pointer_cast<CdsItem>(obj)->getMimeType(), i); });
        if (mark) {
            obj->setFlag(OBJECT_FLAG_PLAYED);

            bool supress = config->getBoolOption(CFG_SERVER_EXTOPTS_MARK_PLAYED_ITEMS_SUPPRESS_CDS_UPDATES);
            log_debug("Marking object {} as played", obj->getTitle().c_str());
            updateObject(obj, !supress);
        }
    }

#ifdef HAVE_LASTFMLIB
    if (config->getBoolOption(CFG_SERVER_EXTOPTS_LASTFM_ENABLED) && startswith(std::static_pointer_cast<CdsItem>(obj)->getMimeType(), ("audio"))) {
        last_fm->startedPlaying(std::static_pointer_cast<CdsItem>(obj));
    }
#endif
    log_debug("end");
}

CMAddFileTask::CMAddFileTask(std::shared_ptr<ContentManager> content,
    fs::directory_entry dirEnt, fs::path rootpath, AutoScanSetting& asSetting, bool cancellable)
    : GenericTask(ContentManagerTask)
    , content(std::move(content))
    , dirEnt(std::move(dirEnt))
    , rootpath(std::move(rootpath))
    , asSetting(asSetting)
{
    this->cancellable = cancellable;
    this->taskType = AddFile;
    if (this->asSetting.adir != nullptr)
        this->asSetting.adir->incTaskCount();
}

fs::path CMAddFileTask::getPath() { return dirEnt.path(); }

fs::path CMAddFileTask::getRootPath() { return rootpath; }

void CMAddFileTask::run()
{
    log_debug("running add file task with path {} recursive: {}", dirEnt.path().c_str(), asSetting.recursive);
    auto self = shared_from_this();
    content->_addFile(dirEnt, rootpath, asSetting, self);
    if (asSetting.adir != nullptr) {
        asSetting.adir->decTaskCount();
        if (asSetting.adir->updateLMT()) {
            log_debug("CMAddFileTask::run: Updating last_modified for autoscan directory {}", asSetting.adir->getLocation().c_str());
            content->getContext()->getDatabase()->updateAutoscanDirectory(asSetting.adir);
        }
    }
}

CMRemoveObjectTask::CMRemoveObjectTask(std::shared_ptr<ContentManager> content, std::shared_ptr<AutoscanDirectory> adir,
    int objectID, bool rescanResource, bool all)
    : GenericTask(ContentManagerTask)
    , content(std::move(content))
    , adir(std::move(adir))
    , objectID(objectID)
    , all(all)
    , rescanResource(rescanResource)
{
    this->taskType = RemoveObject;
    cancellable = false;
}

void CMRemoveObjectTask::run()
{
    content->_removeObject(adir, objectID, rescanResource, all);
}

CMRescanDirectoryTask::CMRescanDirectoryTask(std::shared_ptr<ContentManager> content,
    std::shared_ptr<AutoscanDirectory> adir, int containerId, bool cancellable)
    : GenericTask(ContentManagerTask)
    , content(std::move(content))
    , adir(std::move(adir))
    , containerID(containerId)
{
    this->cancellable = cancellable;
    this->taskType = RescanDirectory;
}

void CMRescanDirectoryTask::run()
{
    if (adir == nullptr)
        return;

    auto self = shared_from_this();
    content->_rescanDirectory(adir, containerID, self);
    adir->decTaskCount();
    if (adir->updateLMT()) {
        log_debug("CMRescanDirectoryTask::run: Updating last_modified for autoscan directory {}", adir->getLocation().c_str());
        content->getContext()->getDatabase()->updateAutoscanDirectory(adir);
    }
}

#ifdef ONLINE_SERVICES
CMFetchOnlineContentTask::CMFetchOnlineContentTask(std::shared_ptr<ContentManager> content,
    std::shared_ptr<TaskProcessor> task_processor, std::shared_ptr<Timer> timer,
    std::shared_ptr<OnlineService> service, std::shared_ptr<Layout> layout, bool cancellable, bool unscheduled_refresh)
    : GenericTask(ContentManagerTask)
    , content(std::move(content))
    , task_processor(std::move(task_processor))
    , timer(std::move(timer))
    , service(std::move(service))
    , layout(std::move(layout))
{
    this->cancellable = cancellable;
    this->unscheduled_refresh = unscheduled_refresh;
    this->taskType = FetchOnlineContent;
}

void CMFetchOnlineContentTask::run()
{
    if (this->service == nullptr) {
        log_debug("Received invalid service!");
        return;
    }
    try {
        std::shared_ptr<GenericTask> t(
            new TPFetchOnlineContentTask(content, task_processor, timer, service, layout, cancellable, unscheduled_refresh));
        task_processor->addTask(t);
    } catch (const std::runtime_error& ex) {
        log_error("{}", ex.what());
    }
}
#endif // ONLINE_SERVICES

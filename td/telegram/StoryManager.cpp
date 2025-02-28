//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2023
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/StoryManager.h"

#include "td/telegram/AccessRights.h"
#include "td/telegram/AuthManager.h"
#include "td/telegram/ConfigManager.h"
#include "td/telegram/ContactsManager.h"
#include "td/telegram/Dependencies.h"
#include "td/telegram/FileReferenceManager.h"
#include "td/telegram/files/FileManager.h"
#include "td/telegram/Global.h"
#include "td/telegram/logevent/LogEvent.h"
#include "td/telegram/logevent/LogEventHelper.h"
#include "td/telegram/MessageEntity.h"
#include "td/telegram/MessagesManager.h"
#include "td/telegram/NotificationId.h"
#include "td/telegram/NotificationManager.h"
#include "td/telegram/OptionManager.h"
#include "td/telegram/ReportReason.h"
#include "td/telegram/StoryContent.h"
#include "td/telegram/StoryContentType.h"
#include "td/telegram/StoryInteractionInfo.hpp"
#include "td/telegram/Td.h"
#include "td/telegram/TdDb.h"
#include "td/telegram/telegram_api.h"
#include "td/telegram/UpdatesManager.h"
#include "td/telegram/UserId.h"
#include "td/telegram/WebPagesManager.h"

#include "td/db/binlog/BinlogEvent.h"
#include "td/db/binlog/BinlogHelper.h"

#include "td/actor/MultiPromise.h"

#include "td/utils/algorithm.h"
#include "td/utils/buffer.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/Random.h"
#include "td/utils/Status.h"
#include "td/utils/Time.h"
#include "td/utils/tl_helpers.h"

#include <limits>

namespace td {

class GetAllStoriesQuery final : public Td::ResultHandler {
  Promise<telegram_api::object_ptr<telegram_api::stories_AllStories>> promise_;

 public:
  explicit GetAllStoriesQuery(Promise<telegram_api::object_ptr<telegram_api::stories_AllStories>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(StoryListId story_list_id, bool is_next, const string &state) {
    int32 flags = 0;
    if (!state.empty()) {
      flags |= telegram_api::stories_getAllStories::STATE_MASK;
    }
    if (is_next) {
      flags |= telegram_api::stories_getAllStories::NEXT_MASK;
    }
    if (story_list_id == StoryListId::archive()) {
      flags |= telegram_api::stories_getAllStories::HIDDEN_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::stories_getAllStories(flags, false /*ignored*/, false /*ignored*/, state)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_getAllStories>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for GetAllStoriesQuery: " << to_string(result);
    promise_.set_value(std::move(result));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class ToggleStoriesHiddenQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  UserId user_id_;
  bool are_hidden_ = false;

 public:
  explicit ToggleStoriesHiddenQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(UserId user_id, bool are_hidden) {
    user_id_ = user_id;
    are_hidden_ = are_hidden;
    auto r_input_user = td_->contacts_manager_->get_input_user(user_id_);
    if (r_input_user.is_error()) {
      return on_error(r_input_user.move_as_error());
    }
    send_query(G()->net_query_creator().create(
        telegram_api::contacts_toggleStoriesHidden(r_input_user.move_as_ok(), are_hidden), {{user_id}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::contacts_toggleStoriesHidden>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for ToggleStoriesHiddenQuery: " << result;
    if (result) {
      td_->contacts_manager_->on_update_user_stories_hidden(user_id_, are_hidden_);
    }
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetAllReadUserStoriesQuery final : public Td::ResultHandler {
 public:
  void send() {
    send_query(G()->net_query_creator().create(telegram_api::stories_getAllReadUserStories()));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_getAllReadUserStories>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for GetAllReadUserStoriesQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), Promise<Unit>());
  }

  void on_error(Status status) final {
    LOG(INFO) << "Receive error for GetAllReadUserStoriesQuery: " << status;
  }
};

class ToggleAllStoriesHiddenQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit ToggleAllStoriesHiddenQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(bool all_stories_hidden) {
    send_query(
        G()->net_query_creator().create(telegram_api::stories_toggleAllStoriesHidden(all_stories_hidden), {{"me"}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_toggleAllStoriesHidden>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for ToggleAllStoriesHiddenQuery: " << result;
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class IncrementStoryViewsQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit IncrementStoryViewsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId owner_dialog_id, const vector<StoryId> &story_ids) {
    CHECK(owner_dialog_id.get_type() == DialogType::User);
    auto r_input_user = td_->contacts_manager_->get_input_user(owner_dialog_id.get_user_id());
    if (r_input_user.is_error()) {
      return on_error(r_input_user.move_as_error());
    }
    send_query(G()->net_query_creator().create(
        telegram_api::stories_incrementStoryViews(r_input_user.move_as_ok(), StoryId::get_input_story_ids(story_ids))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_incrementStoryViews>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class ReadStoriesQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit ReadStoriesQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId owner_dialog_id, StoryId max_read_story_id) {
    CHECK(owner_dialog_id.get_type() == DialogType::User);
    auto r_input_user = td_->contacts_manager_->get_input_user(owner_dialog_id.get_user_id());
    if (r_input_user.is_error()) {
      return on_error(r_input_user.move_as_error());
    }
    send_query(G()->net_query_creator().create(
        telegram_api::stories_readStories(r_input_user.move_as_ok(), max_read_story_id.get())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_readStories>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetStoryViewsListQuery final : public Td::ResultHandler {
  Promise<telegram_api::object_ptr<telegram_api::stories_storyViewsList>> promise_;

 public:
  explicit GetStoryViewsListQuery(Promise<telegram_api::object_ptr<telegram_api::stories_storyViewsList>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(StoryId story_id, int32 offset_date, int64 offset_user_id, int32 limit) {
    send_query(G()->net_query_creator().create(
        telegram_api::stories_getStoryViewsList(story_id.get(), offset_date, offset_user_id, limit)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_getStoryViewsList>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(result_ptr.move_as_ok());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetStoriesByIDQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  UserId user_id_;
  vector<StoryId> story_ids_;

 public:
  explicit GetStoriesByIDQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(UserId user_id, vector<StoryId> story_ids) {
    user_id_ = user_id;
    story_ids_ = std::move(story_ids);
    auto r_input_user = td_->contacts_manager_->get_input_user(user_id_);
    if (r_input_user.is_error()) {
      return on_error(r_input_user.move_as_error());
    }
    send_query(G()->net_query_creator().create(
        telegram_api::stories_getStoriesByID(r_input_user.move_as_ok(), StoryId::get_input_story_ids(story_ids_))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_getStoriesByID>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for GetStoriesByIDQuery: " << to_string(result);
    td_->story_manager_->on_get_stories(DialogId(user_id_), std::move(story_ids_), std::move(result));
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetPinnedStoriesQuery final : public Td::ResultHandler {
  Promise<telegram_api::object_ptr<telegram_api::stories_stories>> promise_;

 public:
  explicit GetPinnedStoriesQuery(Promise<telegram_api::object_ptr<telegram_api::stories_stories>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(UserId user_id, StoryId offset_story_id, int32 limit) {
    auto r_input_user = td_->contacts_manager_->get_input_user(user_id);
    if (r_input_user.is_error()) {
      return on_error(r_input_user.move_as_error());
    }
    send_query(G()->net_query_creator().create(
        telegram_api::stories_getPinnedStories(r_input_user.move_as_ok(), offset_story_id.get(), limit)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_getPinnedStories>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for GetPinnedStoriesQuery: " << to_string(result);
    promise_.set_value(std::move(result));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetStoriesArchiveQuery final : public Td::ResultHandler {
  Promise<telegram_api::object_ptr<telegram_api::stories_stories>> promise_;

 public:
  explicit GetStoriesArchiveQuery(Promise<telegram_api::object_ptr<telegram_api::stories_stories>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(StoryId offset_story_id, int32 limit) {
    send_query(G()->net_query_creator().create(telegram_api::stories_getStoriesArchive(offset_story_id.get(), limit)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_getStoriesArchive>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for GetStoriesArchiveQuery: " << to_string(result);
    promise_.set_value(std::move(result));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetUserStoriesQuery final : public Td::ResultHandler {
  Promise<telegram_api::object_ptr<telegram_api::stories_userStories>> promise_;

 public:
  explicit GetUserStoriesQuery(Promise<telegram_api::object_ptr<telegram_api::stories_userStories>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(UserId user_id) {
    auto r_input_user = td_->contacts_manager_->get_input_user(user_id);
    if (r_input_user.is_error()) {
      return on_error(r_input_user.move_as_error());
    }
    send_query(G()->net_query_creator().create(telegram_api::stories_getUserStories(r_input_user.move_as_ok())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_getUserStories>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for GetUserStoriesQuery: " << to_string(result);
    promise_.set_value(std::move(result));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class EditStoryPrivacyQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit EditStoryPrivacyQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, StoryId story_id, UserPrivacySettingRules &&privacy_rules) {
    int32 flags = telegram_api::stories_editStory::PRIVACY_RULES_MASK;
    send_query(G()->net_query_creator().create(
        telegram_api::stories_editStory(flags, story_id.get(), nullptr, string(),
                                        vector<telegram_api::object_ptr<telegram_api::MessageEntity>>(),
                                        privacy_rules.get_input_privacy_rules(td_)),
        {{StoryFullId{dialog_id, story_id}}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_editStory>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for EditStoryPrivacyQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    if (!td_->auth_manager_->is_bot() && status.message() == "STORY_NOT_MODIFIED") {
      return promise_.set_value(Unit());
    }
    promise_.set_error(std::move(status));
  }
};

class ToggleStoryPinnedQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit ToggleStoryPinnedQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, StoryId story_id, bool is_pinned) {
    send_query(G()->net_query_creator().create(telegram_api::stories_togglePinned({story_id.get()}, is_pinned),
                                               {{StoryFullId{dialog_id, story_id}}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_togglePinned>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for ToggleStoryPinnedQuery: " << ptr;
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class DeleteStoriesQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit DeleteStoriesQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(const vector<StoryId> &story_ids) {
    send_query(
        G()->net_query_creator().create(telegram_api::stories_deleteStories(StoryId::get_input_story_ids(story_ids))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_deleteStories>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for DeleteStoriesQuery: " << ptr;
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetStoriesViewsQuery final : public Td::ResultHandler {
  vector<StoryId> story_ids_;

 public:
  void send(vector<StoryId> story_ids) {
    story_ids_ = std::move(story_ids);
    send_query(G()->net_query_creator().create(
        telegram_api::stories_getStoriesViews(StoryId::get_input_story_ids(story_ids_))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_getStoriesViews>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for GetStoriesViewsQuery: " << to_string(ptr);
    td_->story_manager_->on_get_story_views(story_ids_, std::move(ptr));
  }

  void on_error(Status status) final {
    LOG(INFO) << "Receive error for GetStoriesViewsQuery for " << story_ids_ << ": " << status;
  }
};

class ReportStoryQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit ReportStoryQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(StoryFullId story_full_id, ReportReason &&report_reason) {
    dialog_id_ = story_full_id.get_dialog_id();
    CHECK(dialog_id_.get_type() == DialogType::User);

    auto r_input_user = td_->contacts_manager_->get_input_user(dialog_id_.get_user_id());
    if (r_input_user.is_error()) {
      return on_error(r_input_user.move_as_error());
    }

    send_query(G()->net_query_creator().create(
        telegram_api::stories_report(r_input_user.move_as_ok(), {story_full_id.get_story_id().get()},
                                     report_reason.get_input_report_reason(), report_reason.get_message())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_report>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "ReportStoryQuery");
    promise_.set_error(std::move(status));
  }
};

class StoryManager::SendStoryQuery final : public Td::ResultHandler {
  FileId file_id_;
  unique_ptr<PendingStory> pending_story_;

 public:
  void send(FileId file_id, unique_ptr<PendingStory> pending_story,
            telegram_api::object_ptr<telegram_api::InputFile> input_file) {
    file_id_ = file_id;
    pending_story_ = std::move(pending_story);
    CHECK(pending_story_ != nullptr);

    const auto *story = pending_story_->story_.get();
    const StoryContent *content = story->content_.get();
    auto input_media = get_story_content_input_media(td_, content, std::move(input_file));
    CHECK(input_media != nullptr);

    const FormattedText &caption = story->caption_;
    auto entities = get_input_message_entities(td_->contacts_manager_.get(), &caption, "SendStoryQuery");
    auto privacy_rules = story->privacy_rules_.get_input_privacy_rules(td_);
    auto period = story->expire_date_ - story->date_;
    int32 flags = 0;
    if (!caption.text.empty()) {
      flags |= telegram_api::stories_sendStory::CAPTION_MASK;
    }
    if (!entities.empty()) {
      flags |= telegram_api::stories_sendStory::ENTITIES_MASK;
    }
    if (pending_story_->story_->is_pinned_) {
      flags |= telegram_api::stories_sendStory::PINNED_MASK;
    }
    if (period != 86400) {
      flags |= telegram_api::stories_sendStory::PERIOD_MASK;
    }
    if (story->noforwards_) {
      flags |= telegram_api::stories_sendStory::NOFORWARDS_MASK;
    }

    send_query(G()->net_query_creator().create(
        telegram_api::stories_sendStory(flags, false /*ignored*/, false /*ignored*/, std::move(input_media),
                                        caption.text, std::move(entities), std::move(privacy_rules),
                                        pending_story_->random_id_, period),
        {{pending_story_->dialog_id_}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_sendStory>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for SendStoryQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), Promise<Unit>());

    td_->story_manager_->delete_pending_story(file_id_, std::move(pending_story_), Status::OK());
  }

  void on_error(Status status) final {
    LOG(INFO) << "Receive error for SendStoryQuery: " << status;
    if (G()->close_flag() && G()->use_message_database()) {
      // do not send error, story will be re-sent after restart
      return;
    }

    auto bad_parts = FileManager::get_missing_file_parts(status);
    if (!bad_parts.empty()) {
      td_->story_manager_->on_send_story_file_parts_missing(std::move(pending_story_), std::move(bad_parts));
      return;
    } else {
      td_->story_manager_->delete_pending_story(file_id_, std::move(pending_story_), std::move(status));
    }
  }
};

class StoryManager::EditStoryQuery final : public Td::ResultHandler {
  FileId file_id_;
  unique_ptr<PendingStory> pending_story_;

 public:
  void send(FileId file_id, unique_ptr<PendingStory> pending_story,
            telegram_api::object_ptr<telegram_api::InputFile> input_file, const BeingEditedStory *edited_story) {
    file_id_ = file_id;
    pending_story_ = std::move(pending_story);
    CHECK(pending_story_ != nullptr);

    int32 flags = 0;

    telegram_api::object_ptr<telegram_api::InputMedia> input_media;
    const StoryContent *content = edited_story->content_.get();
    if (content != nullptr) {
      CHECK(input_file != nullptr);
      input_media = get_story_content_input_media(td_, content, std::move(input_file));
      CHECK(input_media != nullptr);
      flags |= telegram_api::stories_editStory::MEDIA_MASK;
    }
    vector<telegram_api::object_ptr<telegram_api::MessageEntity>> entities;
    if (edited_story->edit_caption_) {
      flags |= telegram_api::stories_editStory::CAPTION_MASK;
      flags |= telegram_api::stories_editStory::ENTITIES_MASK;

      entities = get_input_message_entities(td_->contacts_manager_.get(), &edited_story->caption_, "EditStoryQuery");
    }
    send_query(G()->net_query_creator().create(
        telegram_api::stories_editStory(flags, pending_story_->story_id_.get(), std::move(input_media),
                                        edited_story->caption_.text, std::move(entities), Auto()),
        {{StoryFullId{pending_story_->dialog_id_, pending_story_->story_id_}}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stories_editStory>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for EditStoryQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(
        std::move(ptr), PromiseCreator::lambda([file_id = file_id_, pending_story = std::move(pending_story_)](
                                                   Result<Unit> &&result) mutable {
          send_closure(G()->story_manager(), &StoryManager::delete_pending_story, file_id, std::move(pending_story),
                       result.is_ok() ? Status::OK() : result.move_as_error());
        }));
  }

  void on_error(Status status) final {
    LOG(INFO) << "Receive error for EditStoryQuery: " << status;
    if (G()->close_flag() && G()->use_message_database()) {
      // do not send error, story will be edited after restart
      return;
    }

    if (!td_->auth_manager_->is_bot() && status.message() == "STORY_NOT_MODIFIED") {
      return td_->story_manager_->delete_pending_story(file_id_, std::move(pending_story_), Status::OK());
    }

    auto bad_parts = FileManager::get_missing_file_parts(status);
    if (!bad_parts.empty()) {
      td_->story_manager_->on_send_story_file_parts_missing(std::move(pending_story_), std::move(bad_parts));
      return;
    }
    td_->story_manager_->delete_pending_story(file_id_, std::move(pending_story_), std::move(status));
  }
};

class StoryManager::UploadMediaCallback final : public FileManager::UploadCallback {
 public:
  void on_upload_ok(FileId file_id, telegram_api::object_ptr<telegram_api::InputFile> input_file) final {
    send_closure_later(G()->story_manager(), &StoryManager::on_upload_story, file_id, std::move(input_file));
  }
  void on_upload_encrypted_ok(FileId file_id,
                              telegram_api::object_ptr<telegram_api::InputEncryptedFile> input_file) final {
    UNREACHABLE();
  }
  void on_upload_secure_ok(FileId file_id, telegram_api::object_ptr<telegram_api::InputSecureFile> input_file) final {
    UNREACHABLE();
  }
  void on_upload_error(FileId file_id, Status error) final {
    send_closure_later(G()->story_manager(), &StoryManager::on_upload_story_error, file_id, std::move(error));
  }
};

StoryManager::PendingStory::PendingStory(DialogId dialog_id, StoryId story_id, uint32 send_story_num, int64 random_id,
                                         unique_ptr<Story> &&story)
    : dialog_id_(dialog_id)
    , story_id_(story_id)
    , send_story_num_(send_story_num)
    , random_id_(random_id)
    , story_(std::move(story)) {
}

StoryManager::ReadyToSendStory::ReadyToSendStory(FileId file_id, unique_ptr<PendingStory> &&pending_story,
                                                 telegram_api::object_ptr<telegram_api::InputFile> &&input_file)
    : file_id_(file_id), pending_story_(std::move(pending_story)), input_file_(std::move(input_file)) {
}

template <class StorerT>
void StoryManager::Story::store(StorerT &storer) const {
  using td::store;
  bool has_receive_date = receive_date_ != 0;
  bool has_interaction_info = !interaction_info_.is_empty();
  bool has_privacy_rules = privacy_rules_ != UserPrivacySettingRules();
  bool has_content = content_ != nullptr;
  bool has_caption = !caption_.text.empty();
  BEGIN_STORE_FLAGS();
  STORE_FLAG(is_edited_);
  STORE_FLAG(is_pinned_);
  STORE_FLAG(is_public_);
  STORE_FLAG(is_for_close_friends_);
  STORE_FLAG(noforwards_);
  STORE_FLAG(has_receive_date);
  STORE_FLAG(has_interaction_info);
  STORE_FLAG(has_privacy_rules);
  STORE_FLAG(has_content);
  STORE_FLAG(has_caption);
  STORE_FLAG(is_for_contacts_);
  STORE_FLAG(is_for_selected_contacts_);
  END_STORE_FLAGS();
  store(date_, storer);
  store(expire_date_, storer);
  if (has_receive_date) {
    store(receive_date_, storer);
  }
  if (has_interaction_info) {
    store(interaction_info_, storer);
  }
  if (has_privacy_rules) {
    store(privacy_rules_, storer);
  }
  if (has_content) {
    store_story_content(content_.get(), storer);
  }
  if (has_caption) {
    store(caption_, storer);
  }
}

template <class ParserT>
void StoryManager::Story::parse(ParserT &parser) {
  using td::parse;
  bool has_receive_date;
  bool has_interaction_info;
  bool has_privacy_rules;
  bool has_content;
  bool has_caption;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(is_edited_);
  PARSE_FLAG(is_pinned_);
  PARSE_FLAG(is_public_);
  PARSE_FLAG(is_for_close_friends_);
  PARSE_FLAG(noforwards_);
  PARSE_FLAG(has_receive_date);
  PARSE_FLAG(has_interaction_info);
  PARSE_FLAG(has_privacy_rules);
  PARSE_FLAG(has_content);
  PARSE_FLAG(has_caption);
  PARSE_FLAG(is_for_contacts_);
  PARSE_FLAG(is_for_selected_contacts_);
  END_PARSE_FLAGS();
  parse(date_, parser);
  parse(expire_date_, parser);
  if (has_receive_date) {
    parse(receive_date_, parser);
  }
  if (has_interaction_info) {
    parse(interaction_info_, parser);
  }
  if (has_privacy_rules) {
    parse(privacy_rules_, parser);
  }
  if (has_content) {
    parse_story_content(content_, parser);
  }
  if (has_caption) {
    parse(caption_, parser);
  }
}

template <class StorerT>
void StoryManager::StoryInfo::store(StorerT &storer) const {
  using td::store;
  BEGIN_STORE_FLAGS();
  STORE_FLAG(is_for_close_friends_);
  END_STORE_FLAGS();
  store(story_id_, storer);
  store(date_, storer);
  store(expire_date_, storer);
}

template <class ParserT>
void StoryManager::StoryInfo::parse(ParserT &parser) {
  using td::parse;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(is_for_close_friends_);
  END_PARSE_FLAGS();
  parse(story_id_, parser);
  parse(date_, parser);
  parse(expire_date_, parser);
}

template <class StorerT>
void StoryManager::PendingStory::store(StorerT &storer) const {
  using td::store;
  bool is_edit = story_id_.is_server();
  BEGIN_STORE_FLAGS();
  STORE_FLAG(is_edit);
  END_STORE_FLAGS();
  store(dialog_id_, storer);
  if (is_edit) {
    store(story_id_, storer);
  } else {
    store(random_id_, storer);
  }
  store(story_, storer);
}

template <class ParserT>
void StoryManager::PendingStory::parse(ParserT &parser) {
  using td::parse;
  bool is_edit;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(is_edit);
  END_PARSE_FLAGS();
  parse(dialog_id_, parser);
  if (is_edit) {
    parse(story_id_, parser);
  } else {
    parse(random_id_, parser);
  }
  parse(story_, parser);
}

template <class StorerT>
void StoryManager::SavedActiveStories::store(StorerT &storer) const {
  using td::store;
  CHECK(!story_infos_.empty());
  bool has_max_read_story_id = max_read_story_id_ != StoryId();
  BEGIN_STORE_FLAGS();
  STORE_FLAG(has_max_read_story_id);
  END_STORE_FLAGS();
  store(story_infos_, storer);
  if (has_max_read_story_id) {
    store(max_read_story_id_, storer);
  }
}

template <class ParserT>
void StoryManager::SavedActiveStories::parse(ParserT &parser) {
  using td::parse;
  bool has_max_read_story_id;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(has_max_read_story_id);
  END_PARSE_FLAGS();
  parse(story_infos_, parser);
  if (has_max_read_story_id) {
    parse(max_read_story_id_, parser);
  }
}

template <class StorerT>
void StoryManager::SavedStoryList::store(StorerT &storer) const {
  using td::store;
  BEGIN_STORE_FLAGS();
  STORE_FLAG(has_more_);
  END_STORE_FLAGS();
  store(state_, storer);
  store(total_count_, storer);
}

template <class ParserT>
void StoryManager::SavedStoryList::parse(ParserT &parser) {
  using td::parse;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(has_more_);
  END_PARSE_FLAGS();
  parse(state_, parser);
  parse(total_count_, parser);
}

StoryManager::StoryManager(Td *td, ActorShared<> parent) : td_(td), parent_(std::move(parent)) {
  upload_media_callback_ = std::make_shared<UploadMediaCallback>();

  story_reload_timeout_.set_callback(on_story_reload_timeout_callback);
  story_reload_timeout_.set_callback_data(static_cast<void *>(this));

  story_expire_timeout_.set_callback(on_story_expire_timeout_callback);
  story_expire_timeout_.set_callback_data(static_cast<void *>(this));

  story_can_get_viewers_timeout_.set_callback(on_story_can_get_viewers_timeout_callback);
  story_can_get_viewers_timeout_.set_callback_data(static_cast<void *>(this));

  if (G()->use_message_database() && td_->auth_manager_->is_authorized() && !td_->auth_manager_->is_bot()) {
    for (auto story_list_id : {StoryListId::main(), StoryListId::archive()}) {
      auto r_value = G()->td_db()->get_story_db_sync()->get_active_story_list_state(story_list_id);
      if (r_value.is_ok() && !r_value.ok().empty()) {
        SavedStoryList saved_story_list;
        auto status = log_event_parse(saved_story_list, r_value.ok().as_slice());
        if (status.is_error()) {
          LOG(ERROR) << "Load invalid state for " << story_list_id << " from database";
        } else {
          LOG(INFO) << "Load state for " << story_list_id << " from database: " << saved_story_list.state_;
          auto &story_list = get_story_list(story_list_id);
          story_list.state_ = std::move(saved_story_list.state_);
          story_list.server_total_count_ = td::max(saved_story_list.total_count_, 0);
          story_list.server_has_more_ = saved_story_list.has_more_;
          story_list.database_has_more_ = true;
        }
      }
    }
  }
}

StoryManager::~StoryManager() {
  Scheduler::instance()->destroy_on_scheduler(G()->get_gc_scheduler_id(), story_full_id_to_file_source_id_, stories_,
                                              stories_by_global_id_, inaccessible_story_full_ids_,
                                              deleted_story_full_ids_, failed_to_load_story_full_ids_, story_messages_,
                                              active_stories_, max_read_story_ids_, failed_to_load_active_stories_);
}

void StoryManager::start_up() {
  if (!td_->auth_manager_->is_authorized()) {
    return;
  }

  try_synchronize_archive_all_stories();
  load_expired_database_stories();

  for (auto story_list_id : {StoryListId::main(), StoryListId::archive()}) {
    update_story_list_sent_total_count(story_list_id);
  }
}

void StoryManager::timeout_expired() {
  load_expired_database_stories();
}

void StoryManager::hangup() {
  auto fail_promise_map = [](auto &queries) {
    while (!queries.empty()) {
      auto it = queries.begin();
      auto promises = std::move(it->second);
      queries.erase(it);
      fail_promises(promises, Global::request_aborted_error());
    }
  };
  fail_promise_map(reload_story_queries_);

  stop();
}

void StoryManager::tear_down() {
  parent_.reset();
}

void StoryManager::on_story_reload_timeout_callback(void *story_manager_ptr, int64 story_global_id) {
  if (G()->close_flag()) {
    return;
  }

  auto story_manager = static_cast<StoryManager *>(story_manager_ptr);
  send_closure_later(story_manager->actor_id(story_manager), &StoryManager::on_story_reload_timeout, story_global_id);
}

void StoryManager::on_story_reload_timeout(int64 story_global_id) {
  if (G()->close_flag()) {
    return;
  }

  auto story_full_id = stories_by_global_id_.get(story_global_id);
  auto story = get_story(story_full_id);
  if (story == nullptr || opened_stories_.count(story_full_id) == 0) {
    LOG(INFO) << "There is no need to reload " << story_full_id;
    return;
  }

  reload_story(story_full_id, Promise<Unit>(), "on_story_reload_timeout");
  story_reload_timeout_.set_timeout_in(story_global_id, OPENED_STORY_POLL_PERIOD);
}

void StoryManager::on_story_expire_timeout_callback(void *story_manager_ptr, int64 story_global_id) {
  if (G()->close_flag()) {
    return;
  }

  auto story_manager = static_cast<StoryManager *>(story_manager_ptr);
  send_closure_later(story_manager->actor_id(story_manager), &StoryManager::on_story_expire_timeout, story_global_id);
}

void StoryManager::on_story_expire_timeout(int64 story_global_id) {
  if (G()->close_flag()) {
    return;
  }

  auto story_full_id = stories_by_global_id_.get(story_global_id);
  auto story = get_story(story_full_id);
  if (story == nullptr) {
    return;
  }
  if (is_active_story(story)) {
    // timeout used monotonic time instead of wall clock time
    LOG(INFO) << "Receive timeout for non-expired " << story_full_id << ": expire_date = " << story->expire_date_
              << ", current time = " << G()->unix_time();
    return on_story_changed(story_full_id, story, false, false);
  }

  LOG(INFO) << "Have expired " << story_full_id;
  auto owner_dialog_id = story_full_id.get_dialog_id();
  CHECK(owner_dialog_id.is_valid());
  if (!is_story_owned(owner_dialog_id) && story->content_ != nullptr && !story->is_pinned_) {
    // non-owned expired non-pinned stories are fully deleted
    on_delete_story(story_full_id);
  }

  auto active_stories = get_active_stories(owner_dialog_id);
  if (active_stories != nullptr && contains(active_stories->story_ids_, story_full_id.get_story_id())) {
    auto story_ids = active_stories->story_ids_;
    on_update_active_stories(owner_dialog_id, active_stories->max_read_story_id_, std::move(story_ids), Promise<Unit>(),
                             "on_story_expire_timeout");
  }
}

void StoryManager::on_story_can_get_viewers_timeout_callback(void *story_manager_ptr, int64 story_global_id) {
  if (G()->close_flag()) {
    return;
  }

  auto story_manager = static_cast<StoryManager *>(story_manager_ptr);
  send_closure_later(story_manager->actor_id(story_manager), &StoryManager::on_story_can_get_viewers_timeout,
                     story_global_id);
}

void StoryManager::on_story_can_get_viewers_timeout(int64 story_global_id) {
  if (G()->close_flag()) {
    return;
  }

  auto story_full_id = stories_by_global_id_.get(story_global_id);
  auto story = get_story(story_full_id);
  if (story == nullptr) {
    return;
  }

  LOG(INFO) << "Have expired viewers in " << story_full_id;
  if (can_get_story_viewers(story_full_id, story).is_ok()) {
    // timeout used monotonic time instead of wall clock time
    LOG(INFO) << "Receive timeout for " << story_full_id
              << " with available viewers: expire_date = " << story->expire_date_
              << ", current time = " << G()->unix_time();
    return on_story_changed(story_full_id, story, false, false);
  }
  if (story->content_ != nullptr && story->is_update_sent_) {
    // can_get_viewers flag has changed
    send_update_story(story_full_id, story);
  }
  cached_story_viewers_.erase(story_full_id);
}

void StoryManager::load_expired_database_stories() {
  if (!G()->use_message_database()) {
    return;
  }

  LOG(INFO) << "Load " << load_expired_database_stories_next_limit_ << " expired stories";
  G()->td_db()->get_story_db_async()->get_expiring_stories(
      G()->unix_time() - 1, load_expired_database_stories_next_limit_,
      PromiseCreator::lambda([actor_id = actor_id(this)](Result<vector<StoryDbStory>> r_stories) {
        if (G()->close_flag()) {
          return;
        }
        CHECK(r_stories.is_ok());
        send_closure(actor_id, &StoryManager::on_load_expired_database_stories, r_stories.move_as_ok());
      }));
}

void StoryManager::on_load_expired_database_stories(vector<StoryDbStory> stories) {
  if (G()->close_flag()) {
    return;
  }

  int32 next_request_delay;
  if (stories.size() == static_cast<size_t>(load_expired_database_stories_next_limit_)) {
    CHECK(load_expired_database_stories_next_limit_ < (1 << 30));
    load_expired_database_stories_next_limit_ *= 2;
    next_request_delay = 1;
  } else {
    load_expired_database_stories_next_limit_ = DEFAULT_LOADED_EXPIRED_STORIES;
    next_request_delay = Random::fast(300, 420);
  }
  set_timeout_in(next_request_delay);

  LOG(INFO) << "Receive " << stories.size() << " expired stories with next request in " << next_request_delay
            << " seconds";
  for (auto &database_story : stories) {
    auto story = parse_story(database_story.story_full_id_, std::move(database_story.data_));
    if (story != nullptr) {
      LOG(ERROR) << "Receive non-expired " << database_story.story_full_id_;
    }
  }
}

bool StoryManager::is_story_owned(DialogId owner_dialog_id) const {
  return owner_dialog_id == DialogId(td_->contacts_manager_->get_my_id());
}

bool StoryManager::is_active_story(const Story *story) {
  return story != nullptr && G()->unix_time() < story->expire_date_;
}

int32 StoryManager::get_story_viewers_expire_date(const Story *story) const {
  return story->expire_date_ +
         narrow_cast<int32>(td_->option_manager_->get_option_integer("story_viewers_expiration_delay", 86400));
}

const StoryManager::Story *StoryManager::get_story(StoryFullId story_full_id) const {
  return stories_.get_pointer(story_full_id);
}

StoryManager::Story *StoryManager::get_story_editable(StoryFullId story_full_id) {
  return stories_.get_pointer(story_full_id);
}

StoryManager::Story *StoryManager::get_story_force(StoryFullId story_full_id, const char *source) {
  if (!story_full_id.is_valid()) {
    return nullptr;
  }

  auto story = get_story_editable(story_full_id);
  if (story != nullptr && story->content_ != nullptr) {
    return story;
  }

  if (!G()->use_message_database() || failed_to_load_story_full_ids_.count(story_full_id) > 0 ||
      is_inaccessible_story(story_full_id) || deleted_story_full_ids_.count(story_full_id) > 0) {
    return nullptr;
  }

  LOG(INFO) << "Trying to load " << story_full_id << " from database from " << source;

  auto r_value = G()->td_db()->get_story_db_sync()->get_story(story_full_id);
  if (r_value.is_error()) {
    failed_to_load_story_full_ids_.insert(story_full_id);
    return nullptr;
  }
  return on_get_story_from_database(story_full_id, r_value.ok(), source);
}

unique_ptr<StoryManager::Story> StoryManager::parse_story(StoryFullId story_full_id, const BufferSlice &value) {
  auto story = make_unique<Story>();
  auto status = log_event_parse(*story, value.as_slice());
  if (status.is_error()) {
    LOG(ERROR) << "Receive invalid " << story_full_id << " from database: " << status << ' '
               << format::as_hex_dump<4>(value.as_slice());
    delete_story_from_database(story_full_id);
    reload_story(story_full_id, Auto(), "parse_story");
    return nullptr;
  }
  if (story->content_ == nullptr) {
    LOG(ERROR) << "Receive " << story_full_id << " without content from database";
    delete_story_from_database(story_full_id);
    return nullptr;
  }

  auto owner_dialog_id = story_full_id.get_dialog_id();
  if (is_active_story(story.get())) {
    auto active_stories = get_active_stories(owner_dialog_id);
    if (active_stories != nullptr && !contains(active_stories->story_ids_, story_full_id.get_story_id())) {
      LOG(INFO) << "Ignore unavailable active " << story_full_id << " from database";
      delete_story_files(story.get());
      delete_story_from_database(story_full_id);
      return nullptr;
    }
  } else {
    if (!is_story_owned(owner_dialog_id) && !story->is_pinned_) {
      // non-owned expired non-pinned stories are fully deleted
      LOG(INFO) << "Delete expired " << story_full_id;
      delete_story_files(story.get());
      delete_story_from_database(story_full_id);
      return nullptr;
    }
  }

  return story;
}

StoryManager::Story *StoryManager::on_get_story_from_database(StoryFullId story_full_id, const BufferSlice &value,
                                                              const char *source) {
  auto old_story = get_story_editable(story_full_id);
  if (old_story != nullptr && old_story->content_ != nullptr) {
    return old_story;
  }

  if (value.empty()) {
    failed_to_load_story_full_ids_.insert(story_full_id);
    return nullptr;
  }

  auto story = parse_story(story_full_id, value);
  if (story == nullptr) {
    failed_to_load_story_full_ids_.insert(story_full_id);
    return nullptr;
  }

  Dependencies dependencies;
  add_story_dependencies(dependencies, story.get());
  if (!dependencies.resolve_force(td_, "on_get_story_from_database")) {
    reload_story(story_full_id, Auto(), "on_get_story_from_database");
    failed_to_load_story_full_ids_.insert(story_full_id);
    return nullptr;
  }

  LOG(INFO) << "Load new " << story_full_id << " from " << source;

  auto result = story.get();
  stories_.set(story_full_id, std::move(story));
  register_story_global_id(story_full_id, result);

  CHECK(!is_inaccessible_story(story_full_id));
  CHECK(being_edited_stories_.count(story_full_id) == 0);

  on_story_changed(story_full_id, result, true, false, true);

  return result;
}

const StoryManager::ActiveStories *StoryManager::get_active_stories(DialogId owner_dialog_id) const {
  return active_stories_.get_pointer(owner_dialog_id);
}

StoryManager::ActiveStories *StoryManager::get_active_stories_editable(DialogId owner_dialog_id) {
  return active_stories_.get_pointer(owner_dialog_id);
}

StoryManager::ActiveStories *StoryManager::get_active_stories_force(DialogId owner_dialog_id, const char *source) {
  auto active_stories = get_active_stories_editable(owner_dialog_id);
  if (active_stories != nullptr) {
    return active_stories;
  }

  if (!G()->use_message_database() || failed_to_load_active_stories_.count(owner_dialog_id) > 0 ||
      !owner_dialog_id.is_valid()) {
    return nullptr;
  }

  LOG(INFO) << "Trying to load active stories of " << owner_dialog_id << " from database from " << source;
  auto r_value = G()->td_db()->get_story_db_sync()->get_active_stories(owner_dialog_id);
  if (r_value.is_error()) {
    failed_to_load_active_stories_.insert(owner_dialog_id);
    return nullptr;
  }
  return on_get_active_stories_from_database(StoryListId(), owner_dialog_id, r_value.ok(), source);
}

StoryManager::ActiveStories *StoryManager::on_get_active_stories_from_database(StoryListId story_list_id,
                                                                               DialogId owner_dialog_id,
                                                                               const BufferSlice &value,
                                                                               const char *source) {
  auto active_stories = get_active_stories_editable(owner_dialog_id);
  if (active_stories != nullptr) {
    return active_stories;
  }

  if (value.empty()) {
    failed_to_load_active_stories_.insert(owner_dialog_id);
    return nullptr;
  }

  SavedActiveStories saved_active_stories;
  auto status = log_event_parse(saved_active_stories, value.as_slice());
  if (status.is_error()) {
    LOG(ERROR) << "Receive invalid active stories in " << owner_dialog_id << " from database: " << status << ' '
               << format::as_hex_dump<4>(value.as_slice());
    save_active_stories(owner_dialog_id, nullptr, Promise<Unit>(), "on_get_active_stories_from_database");
    failed_to_load_active_stories_.insert(owner_dialog_id);
    return nullptr;
  }

  vector<StoryId> story_ids;
  for (auto &story_info : saved_active_stories.story_infos_) {
    story_ids.push_back(on_get_story_info(owner_dialog_id, std::move(story_info)));
  }

  on_update_active_stories(owner_dialog_id, saved_active_stories.max_read_story_id_, std::move(story_ids),
                           Promise<Unit>(), "on_get_active_stories_from_database", true);

  active_stories = get_active_stories_editable(owner_dialog_id);
  if (active_stories == nullptr) {
    if (!story_list_id.is_valid()) {
      story_list_id = get_dialog_story_list_id(owner_dialog_id);
    }
    if (story_list_id.is_valid()) {
      auto &story_list = get_story_list(story_list_id);
      if (!story_list.is_reloaded_server_total_count_ &&
          story_list.server_total_count_ > static_cast<int32>(story_list.ordered_stories_.size())) {
        story_list.server_total_count_--;
        update_story_list_sent_total_count(story_list_id, story_list);
        save_story_list(story_list_id, story_list.state_, story_list.server_total_count_, story_list.server_has_more_);
      }
    }
  }
  return active_stories;
}

void StoryManager::add_story_dependencies(Dependencies &dependencies, const Story *story) {
  story->interaction_info_.add_dependencies(dependencies);
  story->privacy_rules_.add_dependencies(dependencies);
  if (story->content_ != nullptr) {
    add_story_content_dependencies(dependencies, story->content_.get());
  }
  add_formatted_text_dependencies(dependencies, &story->caption_);
}

void StoryManager::add_pending_story_dependencies(Dependencies &dependencies, const PendingStory *pending_story) {
  dependencies.add_dialog_and_dependencies(pending_story->dialog_id_);
  add_story_dependencies(dependencies, pending_story->story_.get());
}

void StoryManager::load_active_stories(StoryListId story_list_id, Promise<Unit> &&promise) {
  if (!story_list_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Story list must be non-empty"));
  }
  auto &story_list = get_story_list(story_list_id);
  if (story_list.list_last_story_date_ == MAX_DIALOG_DATE) {
    return promise.set_error(Status::Error(404, "Not found"));
  }

  if (story_list.database_has_more_) {
    CHECK(G()->use_message_database());
    story_list.load_list_from_database_queries_.push_back(std::move(promise));
    if (story_list.load_list_from_database_queries_.size() == 1u) {
      G()->td_db()->get_story_db_async()->get_active_story_list(
          story_list_id, story_list.last_loaded_database_dialog_date_.get_order(),
          story_list.last_loaded_database_dialog_date_.get_dialog_id(), 10,
          PromiseCreator::lambda(
              [actor_id = actor_id(this), story_list_id](Result<StoryDbGetActiveStoryListResult> &&result) {
                send_closure(actor_id, &StoryManager::on_load_active_stories_from_database, story_list_id,
                             std::move(result));
              }));
    }
    return;
  }

  if (!story_list.server_has_more_) {
    if (story_list.list_last_story_date_ != MAX_DIALOG_DATE) {
      auto min_story_date = story_list.list_last_story_date_;
      story_list.list_last_story_date_ = MAX_DIALOG_DATE;
      for (auto it = story_list.ordered_stories_.upper_bound(min_story_date); it != story_list.ordered_stories_.end();
           ++it) {
        on_dialog_active_stories_order_updated(it->get_dialog_id(), "load_active_stories");
      }
      update_story_list_sent_total_count(story_list_id, story_list);
    }
    return promise.set_error(Status::Error(404, "Not found"));
  }

  load_active_stories_from_server(story_list_id, story_list, !story_list.state_.empty(), std::move(promise));
}

void StoryManager::on_load_active_stories_from_database(StoryListId story_list_id,
                                                        Result<StoryDbGetActiveStoryListResult> result) {
  G()->ignore_result_if_closing(result);
  auto &story_list = get_story_list(story_list_id);
  auto promises = std::move(story_list.load_list_from_database_queries_);
  CHECK(!promises.empty());
  if (result.is_error()) {
    return fail_promises(promises, result.move_as_error());
  }

  auto active_story_list = result.move_as_ok();

  LOG(INFO) << "Load " << active_story_list.active_stories_.size() << " chats with active stories in " << story_list_id
            << " from database";

  Dependencies dependencies;
  for (auto &active_stories_it : active_story_list.active_stories_) {
    dependencies.add_dialog_and_dependencies(active_stories_it.first);
  }
  if (!dependencies.resolve_force(td_, "on_load_active_stories_from_database")) {
    active_story_list.active_stories_.clear();
    story_list.state_.clear();
    story_list.server_has_more_ = true;
  }

  if (active_story_list.active_stories_.empty()) {
    story_list.last_loaded_database_dialog_date_ = MAX_DIALOG_DATE;
    story_list.database_has_more_ = false;
  } else {
    for (auto &active_stories_it : active_story_list.active_stories_) {
      on_get_active_stories_from_database(story_list_id, active_stories_it.first, active_stories_it.second,
                                          "on_load_active_stories_from_database");
    }
    DialogDate max_story_date(active_story_list.next_order_, active_story_list.next_dialog_id_);
    if (story_list.last_loaded_database_dialog_date_ < max_story_date) {
      story_list.last_loaded_database_dialog_date_ = max_story_date;

      if (story_list.list_last_story_date_ < max_story_date) {
        auto min_story_date = story_list.list_last_story_date_;
        story_list.list_last_story_date_ = max_story_date;
        const auto &owner_dialog_ids = dependencies.get_dialog_ids();
        for (auto it = story_list.ordered_stories_.upper_bound(min_story_date);
             it != story_list.ordered_stories_.end() && *it <= max_story_date; ++it) {
          auto dialog_id = it->get_dialog_id();
          if (owner_dialog_ids.count(dialog_id) == 0) {
            on_dialog_active_stories_order_updated(dialog_id, "on_load_active_stories_from_database 1");
          }
        }
        for (auto owner_dialog_id : owner_dialog_ids) {
          on_dialog_active_stories_order_updated(owner_dialog_id, "on_load_active_stories_from_database 2");
        }
      }
    } else {
      LOG(ERROR) << "Last database story date didn't increase";
    }
    update_story_list_sent_total_count(story_list_id, story_list);
  }

  set_promises(promises);
}

void StoryManager::load_active_stories_from_server(StoryListId story_list_id, StoryList &story_list, bool is_next,
                                                   Promise<Unit> &&promise) {
  story_list.load_list_from_server_queries_.push_back(std::move(promise));
  if (story_list.load_list_from_server_queries_.size() == 1u) {
    auto query_promise =
        PromiseCreator::lambda([actor_id = actor_id(this), story_list_id, is_next, state = story_list.state_](
                                   Result<telegram_api::object_ptr<telegram_api::stories_AllStories>> r_all_stories) {
          send_closure(actor_id, &StoryManager::on_load_active_stories_from_server, story_list_id, is_next, state,
                       std::move(r_all_stories));
        });
    td_->create_handler<GetAllStoriesQuery>(std::move(query_promise))->send(story_list_id, is_next, story_list.state_);
  }
}

void StoryManager::reload_active_stories() {
  for (auto story_list_id : {StoryListId::main(), StoryListId::archive()}) {
    load_active_stories_from_server(story_list_id, get_story_list(story_list_id), false, Promise<Unit>());
  }
}

void StoryManager::on_load_active_stories_from_server(
    StoryListId story_list_id, bool is_next, string old_state,
    Result<telegram_api::object_ptr<telegram_api::stories_AllStories>> r_all_stories) {
  G()->ignore_result_if_closing(r_all_stories);
  auto &story_list = get_story_list(story_list_id);
  auto promises = std::move(story_list.load_list_from_server_queries_);
  CHECK(!promises.empty());
  if (r_all_stories.is_error()) {
    return fail_promises(promises, r_all_stories.move_as_error());
  }
  auto all_stories = r_all_stories.move_as_ok();
  switch (all_stories->get_id()) {
    case telegram_api::stories_allStoriesNotModified::ID: {
      auto stories = telegram_api::move_object_as<telegram_api::stories_allStoriesNotModified>(all_stories);
      if (stories->state_.empty()) {
        LOG(ERROR) << "Receive empty state in " << to_string(stories);
      } else {
        story_list.state_ = std::move(stories->state_);
        save_story_list(story_list_id, story_list.state_, story_list.server_total_count_, story_list.server_has_more_);
      }
      break;
    }
    case telegram_api::stories_allStories::ID: {
      auto stories = telegram_api::move_object_as<telegram_api::stories_allStories>(all_stories);
      td_->contacts_manager_->on_get_users(std::move(stories->users_), "on_load_active_stories_from_server");
      if (stories->state_.empty()) {
        LOG(ERROR) << "Receive empty state in " << to_string(stories);
      } else {
        story_list.state_ = std::move(stories->state_);
      }
      story_list.server_total_count_ = max(stories->count_, 0);
      story_list.is_reloaded_server_total_count_ = true;
      if (!stories->has_more_ || stories->user_stories_.empty()) {
        story_list.server_has_more_ = false;
      }

      MultiPromiseActorSafe mpas{"SaveActiveStoryMultiPromiseActor"};
      mpas.add_promise(PromiseCreator::lambda([actor_id = actor_id(this), story_list_id, state = story_list.state_,
                                               server_total_count = story_list.server_total_count_,
                                               has_more = story_list.server_has_more_](Result<Unit> &&result) mutable {
        if (result.is_ok()) {
          send_closure(actor_id, &StoryManager::save_story_list, story_list_id, std::move(state), server_total_count,
                       has_more);
        }
      }));
      auto lock = mpas.get_promise();

      if (stories->user_stories_.empty() && stories->has_more_) {
        LOG(ERROR) << "Receive no stories, but expected more";
        stories->has_more_ = false;
      }

      auto max_story_date = MIN_DIALOG_DATE;
      vector<DialogId> owner_dialog_ids;
      for (auto &user_stories : stories->user_stories_) {
        auto owner_dialog_id = on_get_user_stories(DialogId(), std::move(user_stories), mpas.get_promise());
        auto active_stories = get_active_stories(owner_dialog_id);
        if (active_stories == nullptr) {
          LOG(ERROR) << "Receive invalid stories";
        } else {
          DialogDate story_date(active_stories->private_order_, owner_dialog_id);
          if (max_story_date < story_date) {
            max_story_date = story_date;
          } else {
            LOG(ERROR) << "Receive " << story_date << " after " << max_story_date << " for "
                       << (is_next ? "next" : "first") << " request with state \"" << old_state << "\" in "
                       << story_list_id << " of " << td_->contacts_manager_->get_my_id();
          }
          owner_dialog_ids.push_back(owner_dialog_id);
        }
      }
      if (!stories->has_more_) {
        max_story_date = MAX_DIALOG_DATE;
      }

      vector<DialogId> delete_dialog_ids;
      auto min_story_date = is_next ? story_list.list_last_story_date_ : MIN_DIALOG_DATE;
      for (auto it = story_list.ordered_stories_.upper_bound(min_story_date);
           it != story_list.ordered_stories_.end() && *it <= max_story_date; ++it) {
        auto dialog_id = it->get_dialog_id();
        if (!td::contains(owner_dialog_ids, dialog_id)) {
          delete_dialog_ids.push_back(dialog_id);
        }
      }
      if (story_list.list_last_story_date_ < max_story_date) {
        story_list.list_last_story_date_ = max_story_date;
        for (auto owner_dialog_id : owner_dialog_ids) {
          on_dialog_active_stories_order_updated(owner_dialog_id, "on_load_active_stories_from_server");
        }
      } else if (is_next) {
        LOG(ERROR) << "Last story date didn't increase";
      }
      if (!delete_dialog_ids.empty()) {
        LOG(INFO) << "Delete active stories in " << delete_dialog_ids;
      }
      for (auto dialog_id : delete_dialog_ids) {
        on_update_active_stories(dialog_id, StoryId(), vector<StoryId>(), mpas.get_promise(),
                                 "on_load_active_stories_from_server");
        load_dialog_expiring_stories(dialog_id, 0, "on_load_active_stories_from_server 1");
      }
      update_story_list_sent_total_count(story_list_id, story_list);

      lock.set_value(Unit());
      break;
    }
    default:
      UNREACHABLE();
  }

  set_promises(promises);
}

void StoryManager::save_story_list(StoryListId story_list_id, string state, int32 total_count, bool has_more) {
  if (G()->close_flag() || !G()->use_message_database()) {
    return;
  }

  SavedStoryList saved_story_list;
  saved_story_list.state_ = std::move(state);
  saved_story_list.total_count_ = total_count;
  saved_story_list.has_more_ = has_more;
  G()->td_db()->get_story_db_async()->add_active_story_list_state(story_list_id, log_event_store(saved_story_list),
                                                                  Promise<Unit>());
}

StoryManager::StoryList &StoryManager::get_story_list(StoryListId story_list_id) {
  CHECK(!td_->auth_manager_->is_bot());
  CHECK(story_list_id.is_valid());
  return story_lists_[story_list_id == StoryListId::archive()];
}

const StoryManager::StoryList &StoryManager::get_story_list(StoryListId story_list_id) const {
  CHECK(!td_->auth_manager_->is_bot());
  CHECK(story_list_id.is_valid());
  return story_lists_[story_list_id == StoryListId::archive()];
}

td_api::object_ptr<td_api::updateStoryListChatCount> StoryManager::get_update_story_list_chat_count_object(
    StoryListId story_list_id, const StoryList &story_list) const {
  CHECK(story_list_id.is_valid());
  return td_api::make_object<td_api::updateStoryListChatCount>(story_list_id.get_story_list_object(),
                                                               story_list.sent_total_count_);
}

void StoryManager::update_story_list_sent_total_count(StoryListId story_list_id) {
  if (td_->auth_manager_->is_bot()) {
    return;
  }
  update_story_list_sent_total_count(story_list_id, get_story_list(story_list_id));
}

void StoryManager::update_story_list_sent_total_count(StoryListId story_list_id, StoryList &story_list) {
  if (story_list.server_total_count_ == -1 || td_->auth_manager_->is_bot()) {
    return;
  }
  LOG(INFO) << "Update story list sent total chat count in " << story_list_id;
  auto new_total_count = static_cast<int32>(story_list.ordered_stories_.size());
  if (story_list.list_last_story_date_ != MAX_DIALOG_DATE) {
    new_total_count = max(new_total_count, story_list.server_total_count_);
  }
  if (story_list.sent_total_count_ != new_total_count) {
    story_list.sent_total_count_ = new_total_count;
    send_closure(G()->td(), &Td::send_update, get_update_story_list_chat_count_object(story_list_id, story_list));
  }
}

void StoryManager::reload_all_read_stories() {
  td_->create_handler<GetAllReadUserStoriesQuery>()->send();
}

void StoryManager::try_synchronize_archive_all_stories() {
  if (G()->close_flag()) {
    return;
  }
  if (has_active_synchronize_archive_all_stories_query_) {
    return;
  }
  if (!td_->option_manager_->get_option_boolean("need_synchronize_archive_all_stories")) {
    return;
  }

  has_active_synchronize_archive_all_stories_query_ = true;
  auto archive_all_stories = td_->option_manager_->get_option_boolean("archive_all_stories");

  auto promise = PromiseCreator::lambda([actor_id = actor_id(this), archive_all_stories](Result<Unit> result) {
    send_closure(actor_id, &StoryManager::on_synchronized_archive_all_stories, archive_all_stories, std::move(result));
  });
  td_->create_handler<ToggleAllStoriesHiddenQuery>(std::move(promise))->send(archive_all_stories);
}

void StoryManager::on_synchronized_archive_all_stories(bool set_archive_all_stories, Result<Unit> result) {
  if (G()->close_flag()) {
    return;
  }
  CHECK(has_active_synchronize_archive_all_stories_query_);
  has_active_synchronize_archive_all_stories_query_ = false;

  auto archive_all_stories = td_->option_manager_->get_option_boolean("archive_all_stories");
  if (archive_all_stories != set_archive_all_stories) {
    return try_synchronize_archive_all_stories();
  }
  td_->option_manager_->set_option_empty("need_synchronize_archive_all_stories");

  if (result.is_error()) {
    send_closure(G()->config_manager(), &ConfigManager::reget_app_config, Promise<Unit>());
  }
}

void StoryManager::toggle_dialog_stories_hidden(DialogId dialog_id, StoryListId story_list_id,
                                                Promise<Unit> &&promise) {
  if (!td_->messages_manager_->have_dialog_force(dialog_id, "toggle_dialog_stories_hidden")) {
    return promise.set_error(Status::Error(400, "Story sender not found"));
  }
  if (!td_->messages_manager_->have_input_peer(dialog_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(400, "Can't access the story sender"));
  }
  if (dialog_id.get_type() != DialogType::User) {
    return promise.set_error(Status::Error(400, "Can't archive sender stories"));
  }
  if (story_list_id == get_dialog_story_list_id(dialog_id)) {
    return promise.set_value(Unit());
  }
  if (!story_list_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Story list must be non-empty"));
  }

  td_->create_handler<ToggleStoriesHiddenQuery>(std::move(promise))
      ->send(dialog_id.get_user_id(), story_list_id == StoryListId::archive());
}

void StoryManager::get_dialog_pinned_stories(DialogId owner_dialog_id, StoryId from_story_id, int32 limit,
                                             Promise<td_api::object_ptr<td_api::stories>> &&promise) {
  if (limit <= 0) {
    return promise.set_error(Status::Error(400, "Parameter limit must be positive"));
  }

  if (!td_->messages_manager_->have_dialog_force(owner_dialog_id, "get_dialog_pinned_stories")) {
    return promise.set_error(Status::Error(400, "Story sender not found"));
  }
  if (!td_->messages_manager_->have_input_peer(owner_dialog_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(400, "Can't access the story sender"));
  }
  if (owner_dialog_id.get_type() != DialogType::User) {
    return promise.set_value(td_api::make_object<td_api::stories>());
  }

  if (from_story_id != StoryId() && !from_story_id.is_server()) {
    return promise.set_error(Status::Error(400, "Invalid value of parameter from_story_id specified"));
  }

  auto query_promise =
      PromiseCreator::lambda([actor_id = actor_id(this), owner_dialog_id, promise = std::move(promise)](
                                 Result<telegram_api::object_ptr<telegram_api::stories_stories>> &&result) mutable {
        if (result.is_error()) {
          return promise.set_error(result.move_as_error());
        }
        send_closure(actor_id, &StoryManager::on_get_dialog_pinned_stories, owner_dialog_id, result.move_as_ok(),
                     std::move(promise));
      });
  td_->create_handler<GetPinnedStoriesQuery>(std::move(query_promise))
      ->send(owner_dialog_id.get_user_id(), from_story_id, limit);
}

void StoryManager::on_get_dialog_pinned_stories(DialogId owner_dialog_id,
                                                telegram_api::object_ptr<telegram_api::stories_stories> &&stories,
                                                Promise<td_api::object_ptr<td_api::stories>> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());
  auto result = on_get_stories(owner_dialog_id, {}, std::move(stories));
  if (owner_dialog_id.get_type() == DialogType::User) {
    td_->contacts_manager_->on_update_user_has_pinned_stories(owner_dialog_id.get_user_id(), result.first > 0);
  }
  promise.set_value(get_stories_object(result.first, transform(result.second, [owner_dialog_id](StoryId story_id) {
                                         return StoryFullId(owner_dialog_id, story_id);
                                       })));
}

void StoryManager::get_story_archive(StoryId from_story_id, int32 limit,
                                     Promise<td_api::object_ptr<td_api::stories>> &&promise) {
  if (limit <= 0) {
    return promise.set_error(Status::Error(400, "Parameter limit must be positive"));
  }

  if (from_story_id != StoryId() && !from_story_id.is_server()) {
    return promise.set_error(Status::Error(400, "Invalid value of parameter from_story_id specified"));
  }

  auto query_promise =
      PromiseCreator::lambda([actor_id = actor_id(this), promise = std::move(promise)](
                                 Result<telegram_api::object_ptr<telegram_api::stories_stories>> &&result) mutable {
        if (result.is_error()) {
          return promise.set_error(result.move_as_error());
        }
        send_closure(actor_id, &StoryManager::on_get_story_archive, result.move_as_ok(), std::move(promise));
      });
  td_->create_handler<GetStoriesArchiveQuery>(std::move(query_promise))->send(from_story_id, limit);
}

void StoryManager::on_get_story_archive(telegram_api::object_ptr<telegram_api::stories_stories> &&stories,
                                        Promise<td_api::object_ptr<td_api::stories>> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());
  DialogId dialog_id(td_->contacts_manager_->get_my_id());
  auto result = on_get_stories(dialog_id, {}, std::move(stories));
  promise.set_value(get_stories_object(result.first, transform(result.second, [dialog_id](StoryId story_id) {
                                         return StoryFullId(dialog_id, story_id);
                                       })));
}

void StoryManager::get_dialog_expiring_stories(DialogId owner_dialog_id,
                                               Promise<td_api::object_ptr<td_api::chatActiveStories>> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());
  if (!td_->messages_manager_->have_dialog_force(owner_dialog_id, "get_dialog_expiring_stories")) {
    return promise.set_error(Status::Error(400, "Story sender not found"));
  }
  if (!td_->messages_manager_->have_input_peer(owner_dialog_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(400, "Can't access the story sender"));
  }
  if (owner_dialog_id.get_type() != DialogType::User) {
    return promise.set_value(get_chat_active_stories_object(owner_dialog_id, nullptr));
  }

  auto active_stories = get_active_stories_force(owner_dialog_id, "get_dialog_expiring_stories");
  if (active_stories != nullptr) {
    if (!promise) {
      return promise.set_value(nullptr);
    }
    promise.set_value(get_chat_active_stories_object(owner_dialog_id, active_stories));
    promise = {};
  }

  auto query_promise =
      PromiseCreator::lambda([actor_id = actor_id(this), owner_dialog_id, promise = std::move(promise)](
                                 Result<telegram_api::object_ptr<telegram_api::stories_userStories>> &&result) mutable {
        if (result.is_error()) {
          return promise.set_error(result.move_as_error());
        }
        send_closure(actor_id, &StoryManager::on_get_dialog_expiring_stories, owner_dialog_id, result.move_as_ok(),
                     std::move(promise));
      });
  td_->create_handler<GetUserStoriesQuery>(std::move(query_promise))->send(owner_dialog_id.get_user_id());
}

class StoryManager::LoadDialogExpiringStoriesLogEvent {
 public:
  DialogId dialog_id_;

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(dialog_id_, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(dialog_id_, parser);
  }
};

uint64 StoryManager::save_load_dialog_expiring_stories_log_event(DialogId owner_dialog_id) {
  LoadDialogExpiringStoriesLogEvent log_event{owner_dialog_id};
  return binlog_add(G()->td_db()->get_binlog(), LogEvent::HandlerType::LoadDialogExpiringStories,
                    get_log_event_storer(log_event));
}

void StoryManager::load_dialog_expiring_stories(DialogId owner_dialog_id, uint64 log_event_id, const char *source) {
  if (load_expiring_stories_log_event_ids_.count(owner_dialog_id) > 0) {
    if (log_event_id != 0) {
      binlog_erase(G()->td_db()->get_binlog(), log_event_id);
    }
    return;
  }
  LOG(INFO) << "Load active stories in " << owner_dialog_id << " from " << source;
  if (log_event_id == 0 && G()->use_message_database()) {
    log_event_id = save_load_dialog_expiring_stories_log_event(owner_dialog_id);
  }
  load_expiring_stories_log_event_ids_[owner_dialog_id] = log_event_id;

  // send later to ensure that active stories are inited before sending the request
  auto promise = PromiseCreator::lambda(
      [actor_id = actor_id(this), owner_dialog_id](Result<td_api::object_ptr<td_api::chatActiveStories>> &&) {
        if (!G()->close_flag()) {
          send_closure(actor_id, &StoryManager::on_load_dialog_expiring_stories, owner_dialog_id);
        }
      });
  send_closure_later(actor_id(this), &StoryManager::get_dialog_expiring_stories, owner_dialog_id, std::move(promise));
}

void StoryManager::on_load_dialog_expiring_stories(DialogId owner_dialog_id) {
  if (G()->close_flag()) {
    return;
  }
  auto it = load_expiring_stories_log_event_ids_.find(owner_dialog_id);
  if (it == load_expiring_stories_log_event_ids_.end()) {
    return;
  }
  auto log_event_id = it->second;
  load_expiring_stories_log_event_ids_.erase(it);
  if (log_event_id != 0) {
    binlog_erase(G()->td_db()->get_binlog(), log_event_id);
  }
  LOG(INFO) << "Finished loading of active stories in " << owner_dialog_id;
}

void StoryManager::on_get_dialog_expiring_stories(DialogId owner_dialog_id,
                                                  telegram_api::object_ptr<telegram_api::stories_userStories> &&stories,
                                                  Promise<td_api::object_ptr<td_api::chatActiveStories>> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());
  td_->contacts_manager_->on_get_users(std::move(stories->users_), "on_get_dialog_expiring_stories");
  owner_dialog_id = on_get_user_stories(owner_dialog_id, std::move(stories->stories_), Promise<Unit>());
  if (promise) {
    promise.set_value(get_chat_active_stories_object(owner_dialog_id));
  } else {
    promise.set_value(nullptr);
  }
}

void StoryManager::open_story(DialogId owner_dialog_id, StoryId story_id, Promise<Unit> &&promise) {
  if (!td_->messages_manager_->have_dialog_force(owner_dialog_id, "open_story")) {
    return promise.set_error(Status::Error(400, "Story sender not found"));
  }
  if (!td_->messages_manager_->have_input_peer(owner_dialog_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(400, "Can't access the story sender"));
  }
  if (!story_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Invalid story identifier specified"));
  }

  StoryFullId story_full_id{owner_dialog_id, story_id};
  const Story *story = get_story(story_full_id);
  if (story == nullptr) {
    return promise.set_value(Unit());
  }

  if (is_story_owned(owner_dialog_id) && story_id.is_server()) {
    if (opened_owned_stories_.empty()) {
      schedule_interaction_info_update();
    }
    auto &open_count = opened_owned_stories_[story_full_id];
    if (++open_count == 1) {
      td_->create_handler<GetStoriesViewsQuery>()->send({story_id});
    }
  }

  if (story->content_ == nullptr) {
    return promise.set_value(Unit());
  }

  if (story_id.is_server()) {
    auto &open_count = opened_stories_[story_full_id];
    if (++open_count == 1) {
      CHECK(story->global_id_ > 0);
      story_reload_timeout_.set_timeout_in(story->global_id_,
                                           story->receive_date_ + OPENED_STORY_POLL_PERIOD - G()->unix_time());
    }
  }

  for (auto file_id : get_story_file_ids(story)) {
    td_->file_manager_->check_local_location_async(file_id, true);
  }

  bool is_active = is_active_story(story);
  bool need_increment_story_views = story_id.is_server() && !is_active && story->is_pinned_;
  bool need_read_story = story_id.is_server() && is_active;

  if (need_increment_story_views) {
    auto &story_views = pending_story_views_[owner_dialog_id];
    story_views.story_ids_.insert(story_id);
    if (!story_views.has_query_) {
      increment_story_views(owner_dialog_id, story_views);
    }
  }

  if (need_read_story && on_update_read_stories(owner_dialog_id, story_id)) {
    read_stories_on_server(owner_dialog_id, story_id, 0);
  }

  promise.set_value(Unit());
}

void StoryManager::close_story(DialogId owner_dialog_id, StoryId story_id, Promise<Unit> &&promise) {
  if (!td_->messages_manager_->have_dialog_force(owner_dialog_id, "close_story")) {
    return promise.set_error(Status::Error(400, "Story sender not found"));
  }
  if (!td_->messages_manager_->have_input_peer(owner_dialog_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(400, "Can't access the story sender"));
  }
  if (!story_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Invalid story identifier specified"));
  }

  StoryFullId story_full_id{owner_dialog_id, story_id};
  if (is_story_owned(owner_dialog_id) && story_id.is_server()) {
    auto &open_count = opened_owned_stories_[story_full_id];
    if (open_count == 0) {
      return promise.set_error(Status::Error(400, "The story wasn't opened"));
    }
    if (--open_count == 0) {
      opened_owned_stories_.erase(story_full_id);
      if (opened_owned_stories_.empty()) {
        interaction_info_update_timeout_.cancel_timeout();
      }
    }
  }

  const Story *story = get_story(story_full_id);
  if (story == nullptr) {
    return promise.set_value(Unit());
  }

  if (story_id.is_server()) {
    auto &open_count = opened_stories_[story_full_id];
    if (open_count > 0 && --open_count == 0) {
      opened_stories_.erase(story_full_id);
      story_reload_timeout_.cancel_timeout(story->global_id_);
    }
  }

  promise.set_value(Unit());
}

void StoryManager::view_story_message(StoryFullId story_full_id) {
  if (!story_full_id.get_story_id().is_server()) {
    return;
  }

  const Story *story = get_story_force(story_full_id, "view_story_message");
  if (story == nullptr || story->receive_date_ < G()->unix_time() - VIEWED_STORY_POLL_PERIOD) {
    reload_story(story_full_id, Promise<Unit>(), "view_story_message");
  }
}

void StoryManager::on_story_replied(StoryFullId story_full_id, UserId replier_user_id) {
  if (!replier_user_id.is_valid() || replier_user_id == td_->contacts_manager_->get_my_id() ||
      !story_full_id.get_story_id().is_server()) {
    return;
  }
  const Story *story = get_story_force(story_full_id, "on_story_replied");
  if (story == nullptr || !is_story_owned(story_full_id.get_dialog_id())) {
    return;
  }

  if (story->content_ != nullptr && G()->unix_time() < get_story_viewers_expire_date(story) &&
      story->interaction_info_.definitely_has_no_user(replier_user_id)) {
    td_->create_handler<GetStoriesViewsQuery>()->send({story_full_id.get_story_id()});
  }
}

void StoryManager::schedule_interaction_info_update() {
  if (interaction_info_update_timeout_.has_timeout()) {
    return;
  }

  interaction_info_update_timeout_.set_callback(std::move(update_interaction_info_static));
  interaction_info_update_timeout_.set_callback_data(static_cast<void *>(this));
  interaction_info_update_timeout_.set_timeout_in(10.0);
}

void StoryManager::update_interaction_info_static(void *story_manager) {
  if (G()->close_flag()) {
    return;
  }

  CHECK(story_manager != nullptr);
  static_cast<StoryManager *>(story_manager)->update_interaction_info();
}

void StoryManager::update_interaction_info() {
  if (opened_owned_stories_.empty()) {
    return;
  }
  vector<StoryId> story_ids;
  for (auto &it : opened_owned_stories_) {
    auto story_full_id = it.first;
    CHECK(story_full_id.get_dialog_id() == DialogId(td_->contacts_manager_->get_my_id()));
    story_ids.push_back(story_full_id.get_story_id());
    if (story_ids.size() >= 100) {
      break;
    }
  }
  td_->create_handler<GetStoriesViewsQuery>()->send(std::move(story_ids));
}

void StoryManager::increment_story_views(DialogId owner_dialog_id, PendingStoryViews &story_views) {
  CHECK(!story_views.has_query_);
  vector<StoryId> viewed_story_ids;
  const size_t MAX_VIEWED_STORIES = 200;  // server-side limit
  while (!story_views.story_ids_.empty() && viewed_story_ids.size() < MAX_VIEWED_STORIES) {
    auto story_id_it = story_views.story_ids_.begin();
    viewed_story_ids.push_back(*story_id_it);
    story_views.story_ids_.erase(story_id_it);
  }
  CHECK(!viewed_story_ids.empty());
  story_views.has_query_ = true;
  auto promise = PromiseCreator::lambda([actor_id = actor_id(this), owner_dialog_id](Result<Unit>) {
    send_closure(actor_id, &StoryManager::on_increment_story_views, owner_dialog_id);
  });
  td_->create_handler<IncrementStoryViewsQuery>(std::move(promise))->send(owner_dialog_id, std::move(viewed_story_ids));
}

void StoryManager::on_increment_story_views(DialogId owner_dialog_id) {
  if (G()->close_flag()) {
    return;
  }

  auto &story_views = pending_story_views_[owner_dialog_id];
  CHECK(story_views.has_query_);
  story_views.has_query_ = false;
  if (story_views.story_ids_.empty()) {
    pending_story_views_.erase(owner_dialog_id);
    return;
  }
  increment_story_views(owner_dialog_id, story_views);
}

class StoryManager::ReadStoriesOnServerLogEvent {
 public:
  DialogId dialog_id_;
  StoryId max_story_id_;

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(dialog_id_, storer);
    td::store(max_story_id_, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(dialog_id_, parser);
    td::parse(max_story_id_, parser);
  }
};

uint64 StoryManager::save_read_stories_on_server_log_event(DialogId dialog_id, StoryId max_story_id) {
  ReadStoriesOnServerLogEvent log_event{dialog_id, max_story_id};
  return binlog_add(G()->td_db()->get_binlog(), LogEvent::HandlerType::ReadStoriesOnServer,
                    get_log_event_storer(log_event));
}

void StoryManager::read_stories_on_server(DialogId owner_dialog_id, StoryId story_id, uint64 log_event_id) {
  if (log_event_id == 0 && G()->use_message_database()) {
    log_event_id = save_read_stories_on_server_log_event(owner_dialog_id, story_id);
  }

  td_->create_handler<ReadStoriesQuery>(get_erase_log_event_promise(log_event_id))->send(owner_dialog_id, story_id);
}

Status StoryManager::can_get_story_viewers(StoryFullId story_full_id, const Story *story) const {
  CHECK(story != nullptr);
  if (!is_story_owned(story_full_id.get_dialog_id())) {
    return Status::Error(400, "Story is not outgoing");
  }
  if (!story_full_id.get_story_id().is_server()) {
    return Status::Error(400, "Story is not sent yet");
  }
  if (G()->unix_time() >= get_story_viewers_expire_date(story)) {
    return Status::Error(400, "Story is too old");
  }
  return Status::OK();
}

void StoryManager::get_story_viewers(StoryId story_id, const td_api::messageViewer *offset, int32 limit,
                                     Promise<td_api::object_ptr<td_api::messageViewers>> &&promise) {
  DialogId owner_dialog_id(td_->contacts_manager_->get_my_id());
  StoryFullId story_full_id{owner_dialog_id, story_id};
  const Story *story = get_story(story_full_id);
  if (story == nullptr) {
    return promise.set_error(Status::Error(400, "Story not found"));
  }
  if (limit <= 0) {
    return promise.set_error(Status::Error(400, "Parameter limit must be positive"));
  }
  if (can_get_story_viewers(story_full_id, story).is_error() || story->interaction_info_.get_view_count() == 0) {
    return promise.set_value(td_api::make_object<td_api::messageViewers>());
  }

  int32 offset_date = 0;
  int64 offset_user_id = 0;
  if (offset != nullptr) {
    offset_date = offset->view_date_;
    offset_user_id = offset->user_id_;
  }
  MessageViewer offset_viewer{UserId(offset_user_id), offset_date};

  auto &cached_viewers = cached_story_viewers_[story_full_id];
  if (cached_viewers != nullptr && story->content_ != nullptr &&
      (cached_viewers->total_count_ == story->interaction_info_.get_view_count() || !offset_viewer.is_empty())) {
    auto result = cached_viewers->viewers_.get_sublist(offset_viewer, limit);
    if (!result.is_empty()) {
      // can return the viewers
      // don't need to reget the viewers, because story->interaction_info_.get_view_count() is updated every 10 seconds
      td_->contacts_manager_->on_view_user_active_stories(result.get_user_ids());
      return promise.set_value(result.get_message_viewers_object(td_->contacts_manager_.get()));
    }
  }

  auto query_promise = PromiseCreator::lambda(
      [actor_id = actor_id(this), story_id, offset_viewer, promise = std::move(promise)](
          Result<telegram_api::object_ptr<telegram_api::stories_storyViewsList>> result) mutable {
        send_closure(actor_id, &StoryManager::on_get_story_viewers, story_id, offset_viewer, std::move(result),
                     std::move(promise));
      });

  td_->create_handler<GetStoryViewsListQuery>(std::move(query_promise))
      ->send(story_full_id.get_story_id(), offset_date, offset_user_id, limit);
}

void StoryManager::on_get_story_viewers(
    StoryId story_id, MessageViewer offset,
    Result<telegram_api::object_ptr<telegram_api::stories_storyViewsList>> r_view_list,
    Promise<td_api::object_ptr<td_api::messageViewers>> &&promise) {
  G()->ignore_result_if_closing(r_view_list);
  if (r_view_list.is_error()) {
    return promise.set_error(r_view_list.move_as_error());
  }
  auto view_list = r_view_list.move_as_ok();

  DialogId owner_dialog_id(td_->contacts_manager_->get_my_id());
  CHECK(story_id.is_server());
  StoryFullId story_full_id{owner_dialog_id, story_id};
  Story *story = get_story_editable(story_full_id);
  if (story == nullptr) {
    return promise.set_value(td_api::make_object<td_api::messageViewers>());
  }

  td_->contacts_manager_->on_get_users(std::move(view_list->users_), "on_get_story_viewers");

  auto total_count = view_list->count_;
  if (total_count < 0 || static_cast<size_t>(total_count) < view_list->views_.size()) {
    LOG(ERROR) << "Receive total_count = " << total_count << " and " << view_list->views_.size() << " story viewers";
    total_count = static_cast<int32>(view_list->views_.size());
  }

  MessageViewers story_viewers(std::move(view_list->views_));
  if (story->content_ != nullptr) {
    if (story->interaction_info_.set_view_count(view_list->count_)) {
      if (offset.is_empty()) {
        story->interaction_info_.set_recent_viewer_user_ids(story_viewers.get_user_ids());
      }
      on_story_changed(story_full_id, story, true, true);
    }
    auto &cached_viewers = cached_story_viewers_[story_full_id];
    if (cached_viewers == nullptr) {
      cached_viewers = make_unique<CachedStoryViewers>();
    }
    if (total_count < cached_viewers->total_count_) {
      LOG(ERROR) << "Total viewer count decreased from " << cached_viewers->total_count_ << " to " << total_count;
    } else {
      cached_viewers->total_count_ = total_count;
    }
    cached_viewers->viewers_.add_sublist(offset, story_viewers);
  }

  td_->contacts_manager_->on_view_user_active_stories(story_viewers.get_user_ids());
  promise.set_value(story_viewers.get_message_viewers_object(td_->contacts_manager_.get()));
}

void StoryManager::report_story(StoryFullId story_full_id, ReportReason &&reason, Promise<Unit> &&promise) {
  if (!have_story_force(story_full_id)) {
    return promise.set_error(Status::Error(400, "Story not found"));
  }

  td_->create_handler<ReportStoryQuery>(std::move(promise))->send(story_full_id, std::move(reason));
}

bool StoryManager::have_story(StoryFullId story_full_id) const {
  return get_story(story_full_id) != nullptr;
}

bool StoryManager::have_story_force(StoryFullId story_full_id) {
  return get_story_force(story_full_id, "have_story_force") != nullptr;
}

bool StoryManager::is_inaccessible_story(StoryFullId story_full_id) const {
  return inaccessible_story_full_ids_.count(story_full_id) > 0;
}

int32 StoryManager::get_story_duration(StoryFullId story_full_id) const {
  const Story *story = get_story(story_full_id);
  if (story == nullptr || story->content_ == nullptr) {
    return -1;
  }
  auto *content = story->content_.get();
  auto it = being_edited_stories_.find(story_full_id);
  if (it != being_edited_stories_.end()) {
    if (it->second->content_ != nullptr) {
      content = it->second->content_.get();
    }
  }
  return get_story_content_duration(td_, content);
}

void StoryManager::register_story(StoryFullId story_full_id, FullMessageId full_message_id, const char *source) {
  if (td_->auth_manager_->is_bot()) {
    return;
  }
  CHECK(story_full_id.is_valid());

  LOG(INFO) << "Register " << story_full_id << " from " << full_message_id << " from " << source;
  story_messages_[story_full_id].insert(full_message_id);
}

void StoryManager::unregister_story(StoryFullId story_full_id, FullMessageId full_message_id, const char *source) {
  if (td_->auth_manager_->is_bot()) {
    return;
  }
  CHECK(story_full_id.is_valid());
  LOG(INFO) << "Unregister " << story_full_id << " from " << full_message_id << " from " << source;
  auto &message_ids = story_messages_[story_full_id];
  auto is_deleted = message_ids.erase(full_message_id) > 0;
  LOG_CHECK(is_deleted) << source << ' ' << story_full_id << ' ' << full_message_id;
  if (message_ids.empty()) {
    story_messages_.erase(story_full_id);
  }
}

StoryManager::StoryInfo StoryManager::get_story_info(StoryFullId story_full_id) const {
  const auto *story = get_story(story_full_id);
  if (story == nullptr || !is_active_story(story)) {
    return {};
  }
  StoryInfo story_info;
  story_info.story_id_ = story_full_id.get_story_id();
  story_info.date_ = story->date_;
  story_info.expire_date_ = story->expire_date_;
  story_info.is_for_close_friends_ = story->is_for_close_friends_;
  return story_info;
}

td_api::object_ptr<td_api::storyInfo> StoryManager::get_story_info_object(StoryFullId story_full_id) const {
  auto story_info = get_story_info(story_full_id);
  if (!story_info.story_id_.is_valid()) {
    return nullptr;
  }
  return td_api::make_object<td_api::storyInfo>(story_info.story_id_.get(), story_info.date_,
                                                story_info.is_for_close_friends_);
}

td_api::object_ptr<td_api::story> StoryManager::get_story_object(StoryFullId story_full_id) const {
  return get_story_object(story_full_id, get_story(story_full_id));
}

td_api::object_ptr<td_api::story> StoryManager::get_story_object(StoryFullId story_full_id, const Story *story) const {
  if (story == nullptr || story->content_ == nullptr) {
    return nullptr;
  }
  auto dialog_id = story_full_id.get_dialog_id();
  bool is_owned = is_story_owned(dialog_id);
  if (!is_owned && !story->is_pinned_ && !is_active_story(story)) {
    return nullptr;
  }

  td_api::object_ptr<td_api::StoryPrivacySettings> privacy_settings;
  if (story->is_public_) {
    privacy_settings = td_api::make_object<td_api::storyPrivacySettingsEveryone>();
  } else if (story->is_for_close_friends_) {
    privacy_settings = td_api::make_object<td_api::storyPrivacySettingsCloseFriends>();
  } else {
    privacy_settings = story->privacy_rules_.get_story_privacy_settings_object(td_);
    if (privacy_settings == nullptr) {
      if (story->is_for_contacts_) {
        privacy_settings = td_api::make_object<td_api::storyPrivacySettingsContacts>();
      } else {
        privacy_settings = td_api::make_object<td_api::storyPrivacySettingsSelectedContacts>();
      }
    }
  }

  bool is_being_edited = false;
  bool is_edited = story->is_edited_;

  auto story_id = story_full_id.get_story_id();
  auto *content = story->content_.get();
  auto *caption = &story->caption_;
  if (is_owned && story_id.is_server()) {
    auto it = being_edited_stories_.find(story_full_id);
    if (it != being_edited_stories_.end()) {
      if (it->second->content_ != nullptr) {
        content = it->second->content_.get();
      }
      if (it->second->edit_caption_) {
        caption = &it->second->caption_;
      }
      is_being_edited = true;
    }
  }

  auto changelog_dialog_id = get_changelog_story_dialog_id();
  bool is_visible_only_for_self =
      !story_id.is_server() || dialog_id == changelog_dialog_id || (!story->is_pinned_ && !is_active_story(story));
  bool can_be_forwarded = !story->noforwards_ && story_id.is_server() &&
                          privacy_settings->get_id() == td_api::storyPrivacySettingsEveryone::ID;
  bool can_be_replied = story_id.is_server() && dialog_id != changelog_dialog_id;
  bool can_get_viewers = can_get_story_viewers(story_full_id, story).is_ok();
  bool has_expired_viewers = !can_get_viewers && is_story_owned(dialog_id) && story_id.is_server();

  story->is_update_sent_ = true;

  return td_api::make_object<td_api::story>(
      story_id.get(), td_->messages_manager_->get_chat_id_object(dialog_id, "get_story_object"), story->date_,
      is_being_edited, is_edited, story->is_pinned_, is_visible_only_for_self, can_be_forwarded, can_be_replied,
      can_get_viewers, has_expired_viewers, story->interaction_info_.get_story_interaction_info_object(td_),
      std::move(privacy_settings), get_story_content_object(td_, content),
      get_formatted_text_object(*caption, true, get_story_content_duration(td_, content)));
}

td_api::object_ptr<td_api::stories> StoryManager::get_stories_object(int32 total_count,
                                                                     const vector<StoryFullId> &story_full_ids) const {
  if (total_count == -1) {
    total_count = static_cast<int32>(story_full_ids.size());
  }
  return td_api::make_object<td_api::stories>(total_count, transform(story_full_ids, [this](StoryFullId story_full_id) {
                                                return get_story_object(story_full_id);
                                              }));
}

td_api::object_ptr<td_api::chatActiveStories> StoryManager::get_chat_active_stories_object(
    DialogId owner_dialog_id) const {
  return get_chat_active_stories_object(owner_dialog_id, get_active_stories(owner_dialog_id));
}

td_api::object_ptr<td_api::chatActiveStories> StoryManager::get_chat_active_stories_object(
    DialogId owner_dialog_id, const ActiveStories *active_stories) const {
  StoryListId story_list_id;
  StoryId max_read_story_id;
  vector<td_api::object_ptr<td_api::storyInfo>> stories;
  int64 order = 0;
  if (active_stories != nullptr) {
    story_list_id = active_stories->story_list_id_;
    max_read_story_id = active_stories->max_read_story_id_;
    for (auto story_id : active_stories->story_ids_) {
      auto story_info = get_story_info_object({owner_dialog_id, story_id});
      if (story_info != nullptr) {
        stories.push_back(std::move(story_info));
      }
    }
    if (story_list_id.is_valid()) {
      order = active_stories->public_order_;
    }
  } else {
    story_list_id = get_dialog_story_list_id(owner_dialog_id);
  }
  return td_api::make_object<td_api::chatActiveStories>(
      td_->messages_manager_->get_chat_id_object(owner_dialog_id, "updateChatActiveStories"),
      story_list_id.get_story_list_object(), order, max_read_story_id.get(), std::move(stories));
}

vector<FileId> StoryManager::get_story_file_ids(const Story *story) const {
  if (story == nullptr || story->content_ == nullptr) {
    return {};
  }
  return get_story_content_file_ids(td_, story->content_.get());
}

void StoryManager::delete_story_files(const Story *story) const {
  for (auto file_id : get_story_file_ids(story)) {
    send_closure(G()->file_manager(), &FileManager::delete_file, file_id, Promise<Unit>(), "delete_story_files");
  }
}

void StoryManager::change_story_files(StoryFullId story_full_id, const Story *story,
                                      const vector<FileId> &old_file_ids) {
  auto new_file_ids = get_story_file_ids(story);
  if (new_file_ids == old_file_ids) {
    return;
  }

  for (auto file_id : old_file_ids) {
    if (!td::contains(new_file_ids, file_id)) {
      send_closure(G()->file_manager(), &FileManager::delete_file, file_id, Promise<Unit>(), "change_story_files");
    }
  }

  auto file_source_id = get_story_file_source_id(story_full_id);
  if (file_source_id.is_valid()) {
    td_->file_manager_->change_files_source(file_source_id, old_file_ids, new_file_ids);
  }
}

StoryId StoryManager::on_get_story(DialogId owner_dialog_id,
                                   telegram_api::object_ptr<telegram_api::StoryItem> &&story_item_ptr) {
  if (!owner_dialog_id.is_valid()) {
    LOG(ERROR) << "Receive a story in " << owner_dialog_id;
    return {};
  }
  if (td_->auth_manager_->is_bot()) {
    return {};
  }
  CHECK(story_item_ptr != nullptr);
  switch (story_item_ptr->get_id()) {
    case telegram_api::storyItemDeleted::ID:
      return on_get_deleted_story(owner_dialog_id,
                                  telegram_api::move_object_as<telegram_api::storyItemDeleted>(story_item_ptr));
    case telegram_api::storyItemSkipped::ID:
      LOG(ERROR) << "Receive " << to_string(story_item_ptr);
      return {};
    case telegram_api::storyItem::ID: {
      return on_get_new_story(owner_dialog_id, telegram_api::move_object_as<telegram_api::storyItem>(story_item_ptr));
    }
    default:
      UNREACHABLE();
  }
}

StoryId StoryManager::on_get_new_story(DialogId owner_dialog_id,
                                       telegram_api::object_ptr<telegram_api::storyItem> &&story_item) {
  CHECK(story_item != nullptr);
  StoryId story_id(story_item->id_);
  if (!story_id.is_server()) {
    LOG(ERROR) << "Receive " << to_string(story_item);
    return StoryId();
  }
  CHECK(owner_dialog_id.is_valid());
  StoryFullId story_full_id{owner_dialog_id, story_id};
  if (deleted_story_full_ids_.count(story_full_id) > 0) {
    return StoryId();
  }

  td_->messages_manager_->force_create_dialog(owner_dialog_id, "on_get_new_story");

  bool is_bot = td_->auth_manager_->is_bot();
  auto caption =
      get_message_text(td_->contacts_manager_.get(), std::move(story_item->caption_), std::move(story_item->entities_),
                       true, is_bot, story_item->date_, false, "on_get_new_story");
  auto content = get_story_content(td_, std::move(story_item->media_), owner_dialog_id);
  if (content == nullptr) {
    return StoryId();
  }

  Story *story = get_story_force(story_full_id, "on_get_new_story");
  bool is_changed = false;
  bool need_save_to_database = false;
  if (story == nullptr) {
    auto s = make_unique<Story>();
    story = s.get();
    stories_.set(story_full_id, std::move(s));
    is_changed = true;
    story_item->min_ = false;
    register_story_global_id(story_full_id, story);

    inaccessible_story_full_ids_.erase(story_full_id);
    failed_to_load_story_full_ids_.erase(story_full_id);
    LOG(INFO) << "Add new " << story_full_id;
  }
  CHECK(story != nullptr);

  story->receive_date_ = G()->unix_time();

  const BeingEditedStory *edited_story = nullptr;
  auto it = being_edited_stories_.find(story_full_id);
  if (it != being_edited_stories_.end()) {
    edited_story = it->second.get();
  }

  auto content_type = content->get_type();
  auto old_file_ids = get_story_file_ids(story);
  if (edited_story != nullptr && edited_story->content_ != nullptr) {
    story->content_ = std::move(content);
    need_save_to_database = true;
  } else if (story->content_ == nullptr || story->content_->get_type() != content_type) {
    story->content_ = std::move(content);
    is_changed = true;
  } else {
    merge_story_contents(td_, story->content_.get(), content.get(), owner_dialog_id, need_save_to_database, is_changed);
    story->content_ = std::move(content);
  }

  if (is_changed || need_save_to_database) {
    change_story_files(story_full_id, story, old_file_ids);
  }

  if (story_item->date_ <= 0) {
    LOG(ERROR) << "Receive " << story_full_id << " sent at " << story_item->date_;
    story_item->date_ = 1;
  }
  if (story_item->expire_date_ <= story_item->date_) {
    LOG(ERROR) << "Receive " << story_full_id << " sent at " << story_item->date_ << ", but expired at "
               << story_item->expire_date_;
    story_item->expire_date_ = story_item->date_ + 1;
  }

  if (story->is_edited_ != story_item->edited_ || story->is_pinned_ != story_item->pinned_ ||
      story->is_public_ != story_item->public_ || story->is_for_close_friends_ != story_item->close_friends_ ||
      story->is_for_contacts_ != story_item->contacts_ ||
      story->is_for_selected_contacts_ != story_item->selected_contacts_ ||
      story->noforwards_ != story_item->noforwards_ || story->date_ != story_item->date_ ||
      story->expire_date_ != story_item->expire_date_) {
    story->is_edited_ = story_item->edited_;
    story->is_pinned_ = story_item->pinned_;
    story->is_public_ = story_item->public_;
    story->is_for_close_friends_ = story_item->close_friends_;
    story->is_for_contacts_ = story_item->contacts_;
    story->is_for_selected_contacts_ = story_item->selected_contacts_;
    story->noforwards_ = story_item->noforwards_;
    story->date_ = story_item->date_;
    story->expire_date_ = story_item->expire_date_;
    is_changed = true;
  }
  if (!is_story_owned(owner_dialog_id)) {
    story_item->min_ = false;
  }
  if (!story_item->min_) {
    auto privacy_rules = UserPrivacySettingRules::get_user_privacy_setting_rules(td_, std::move(story_item->privacy_));
    auto interaction_info = StoryInteractionInfo(td_, std::move(story_item->views_));

    if (story->privacy_rules_ != privacy_rules || story->interaction_info_ != interaction_info) {
      story->privacy_rules_ = std::move(privacy_rules);
      story->interaction_info_ = std::move(interaction_info);
      is_changed = true;
    }
  }
  if (story->caption_ != caption) {
    story->caption_ = std::move(caption);
    if (edited_story != nullptr && edited_story->edit_caption_) {
      need_save_to_database = true;
    } else {
      is_changed = true;
    }
  }

  Dependencies dependencies;
  add_story_dependencies(dependencies, story);
  for (auto dependent_dialog_id : dependencies.get_dialog_ids()) {
    td_->messages_manager_->force_create_dialog(dependent_dialog_id, "on_get_new_story", true);
  }

  on_story_changed(story_full_id, story, is_changed, need_save_to_database);

  LOG(INFO) << "Receive " << story_full_id;

  if (is_active_story(story)) {
    auto active_stories = get_active_stories_force(owner_dialog_id, "on_get_new_story");
    if (active_stories == nullptr) {
      if (is_subscribed_to_dialog_stories(owner_dialog_id)) {
        load_dialog_expiring_stories(owner_dialog_id, 0, "on_get_new_story");
      }
    } else if (!contains(active_stories->story_ids_, story_id)) {
      auto story_ids = active_stories->story_ids_;
      story_ids.push_back(story_id);
      size_t i = story_ids.size() - 1;
      while (i > 0 && story_ids[i - 1].get() > story_id.get()) {
        story_ids[i] = story_ids[i - 1];
        i--;
      }
      story_ids[i] = story_id;
      on_update_active_stories(owner_dialog_id, active_stories->max_read_story_id_, std::move(story_ids),
                               Promise<Unit>(), "on_get_new_story");
    }
  }

  return story_id;
}

StoryId StoryManager::on_get_skipped_story(DialogId owner_dialog_id,
                                           telegram_api::object_ptr<telegram_api::storyItemSkipped> &&story_item) {
  CHECK(story_item != nullptr);
  StoryInfo story_info;
  story_info.story_id_ = StoryId(story_item->id_);
  story_info.date_ = story_item->date_;
  story_info.expire_date_ = story_item->expire_date_;
  story_info.is_for_close_friends_ = story_item->close_friends_;
  return on_get_story_info(owner_dialog_id, std::move(story_info));
}

StoryId StoryManager::on_get_story_info(DialogId owner_dialog_id, StoryInfo &&story_info) {
  StoryId story_id = story_info.story_id_;
  if (!story_id.is_server()) {
    LOG(ERROR) << "Receive " << story_id;
    return StoryId();
  }
  if (deleted_story_full_ids_.count({owner_dialog_id, story_id}) > 0) {
    return StoryId();
  }

  td_->messages_manager_->force_create_dialog(owner_dialog_id, "on_get_skipped_story");

  StoryFullId story_full_id{owner_dialog_id, story_id};
  Story *story = get_story_editable(story_full_id);
  if (story == nullptr) {
    auto s = make_unique<Story>();
    story = s.get();
    stories_.set(story_full_id, std::move(s));
    register_story_global_id(story_full_id, story);

    inaccessible_story_full_ids_.erase(story_full_id);
  }
  CHECK(story != nullptr);

  if (story_info.date_ <= 0) {
    LOG(ERROR) << "Receive " << story_full_id << " sent at " << story_info.date_;
    story_info.date_ = 1;
  }
  if (story_info.expire_date_ <= story_info.date_) {
    LOG(ERROR) << "Receive " << story_full_id << " sent at " << story_info.date_ << ", but expired at "
               << story_info.expire_date_;
    story_info.expire_date_ = story_info.date_ + 1;
  }

  if (story->date_ != story_info.date_ || story->expire_date_ != story_info.expire_date_ ||
      story->is_for_close_friends_ != story_info.is_for_close_friends_) {
    story->date_ = story_info.date_;
    story->expire_date_ = story_info.expire_date_;
    story->is_for_close_friends_ = story_info.is_for_close_friends_;
    on_story_changed(story_full_id, story, true, true);
  }
  return story_id;
}

StoryId StoryManager::on_get_deleted_story(DialogId owner_dialog_id,
                                           telegram_api::object_ptr<telegram_api::storyItemDeleted> &&story_item) {
  StoryId story_id(story_item->id_);
  on_delete_story({owner_dialog_id, story_id});
  return story_id;
}

void StoryManager::on_delete_story(StoryFullId story_full_id) {
  auto story_id = story_full_id.get_story_id();
  if (!story_id.is_server()) {
    LOG(ERROR) << "Receive deleted " << story_full_id;
    return;
  }

  inaccessible_story_full_ids_.set(story_full_id, Time::now());
  send_closure_later(G()->messages_manager(),
                     &MessagesManager::update_story_max_reply_media_timestamp_in_replied_messages, story_full_id);

  LOG(INFO) << "Delete " << story_full_id;
  const Story *story = get_story_force(story_full_id, "on_delete_story");
  if (story == nullptr) {
    delete_story_from_database(story_full_id);
    return;
  }
  auto owner_dialog_id = story_full_id.get_dialog_id();
  if (story->is_update_sent_) {
    send_closure(
        G()->td(), &Td::send_update,
        td_api::make_object<td_api::updateStoryDeleted>(
            td_->messages_manager_->get_chat_id_object(owner_dialog_id, "updateStoryDeleted"), story_id.get()));
  }
  delete_story_files(story);
  unregister_story_global_id(story);
  stories_.erase(story_full_id);
  auto edited_stories_it = being_edited_stories_.find(story_full_id);
  if (edited_stories_it != being_edited_stories_.end()) {
    CHECK(edited_stories_it->second != nullptr);
    auto log_event_id = edited_stories_it->second->log_event_id_;
    if (log_event_id != 0) {
      binlog_erase(G()->td_db()->get_binlog(), log_event_id);
    }
    being_edited_stories_.erase(edited_stories_it);
  }
  edit_generations_.erase(story_full_id);
  cached_story_viewers_.erase(story_full_id);

  auto active_stories = get_active_stories_force(owner_dialog_id, "on_get_deleted_story");
  if (active_stories != nullptr && contains(active_stories->story_ids_, story_id)) {
    auto story_ids = active_stories->story_ids_;
    td::remove(story_ids, story_id);
    on_update_active_stories(owner_dialog_id, active_stories->max_read_story_id_, std::move(story_ids), Promise<Unit>(),
                             "on_delete_story");
  }

  delete_story_from_database(story_full_id);
}

void StoryManager::delete_story_from_database(StoryFullId story_full_id) {
  if (G()->use_message_database()) {
    LOG(INFO) << "Delete " << story_full_id << " from database";
    G()->td_db()->get_story_db_async()->delete_story(story_full_id, Promise<Unit>());
  }
}

void StoryManager::on_story_changed(StoryFullId story_full_id, const Story *story, bool is_changed,
                                    bool need_save_to_database, bool from_database) {
  if (is_active_story(story)) {
    CHECK(story->global_id_ > 0);
    story_expire_timeout_.set_timeout_in(story->global_id_, story->expire_date_ - G()->unix_time());
  }
  if (can_get_story_viewers(story_full_id, story).is_ok()) {
    story_can_get_viewers_timeout_.set_timeout_in(story->global_id_,
                                                  get_story_viewers_expire_date(story) - G()->unix_time());
  }
  if (story->content_ == nullptr || !story_full_id.get_story_id().is_valid()) {
    return;
  }
  if (is_changed || need_save_to_database) {
    if (G()->use_message_database() && !from_database) {
      LOG(INFO) << "Add " << story_full_id << " to database";

      int32 expires_at = 0;
      if (is_active_story(story) && !is_story_owned(story_full_id.get_dialog_id()) && !story->is_pinned_) {
        // non-owned expired non-pinned stories must be deleted
        expires_at = story->expire_date_;
      }

      G()->td_db()->get_story_db_async()->add_story(story_full_id, expires_at, NotificationId(),
                                                    log_event_store(*story), Promise<Unit>());
    }

    if (is_changed && story->is_update_sent_) {
      send_update_story(story_full_id, story);
    }

    send_closure_later(G()->messages_manager(),
                       &MessagesManager::update_story_max_reply_media_timestamp_in_replied_messages, story_full_id);
    send_closure_later(G()->web_pages_manager(), &WebPagesManager::on_story_changed, story_full_id);

    if (story_messages_.count(story_full_id) != 0) {
      vector<FullMessageId> full_message_ids;
      story_messages_[story_full_id].foreach(
          [&full_message_ids](const FullMessageId &full_message_id) { full_message_ids.push_back(full_message_id); });
      CHECK(!full_message_ids.empty());
      for (const auto &full_message_id : full_message_ids) {
        td_->messages_manager_->on_external_update_message_content(full_message_id);
      }
    }
  }
}

void StoryManager::register_story_global_id(StoryFullId story_full_id, Story *story) {
  CHECK(story->global_id_ == 0);
  story->global_id_ = ++max_story_global_id_;
  stories_by_global_id_[story->global_id_] = story_full_id;
}

void StoryManager::unregister_story_global_id(const Story *story) {
  CHECK(story->global_id_ > 0);
  stories_by_global_id_.erase(story->global_id_);
}

std::pair<int32, vector<StoryId>> StoryManager::on_get_stories(
    DialogId owner_dialog_id, vector<StoryId> &&expected_story_ids,
    telegram_api::object_ptr<telegram_api::stories_stories> &&stories) {
  td_->contacts_manager_->on_get_users(std::move(stories->users_), "on_get_stories");

  vector<StoryId> story_ids;
  for (auto &story : stories->stories_) {
    switch (story->get_id()) {
      case telegram_api::storyItemDeleted::ID:
        on_get_deleted_story(owner_dialog_id, telegram_api::move_object_as<telegram_api::storyItemDeleted>(story));
        break;
      case telegram_api::storyItemSkipped::ID:
        LOG(ERROR) << "Receive " << to_string(story);
        break;
      case telegram_api::storyItem::ID: {
        auto story_id = on_get_new_story(owner_dialog_id, telegram_api::move_object_as<telegram_api::storyItem>(story));
        if (story_id.is_valid()) {
          story_ids.push_back(story_id);
        }
        break;
      }
      default:
        UNREACHABLE();
    }
  }

  auto total_count = stories->count_;
  if (total_count < static_cast<int32>(story_ids.size())) {
    LOG(ERROR) << "Expected at most " << total_count << " stories, but receive " << story_ids.size();
    total_count = static_cast<int32>(story_ids.size());
  }
  if (!expected_story_ids.empty()) {
    FlatHashSet<StoryId, StoryIdHash> all_story_ids;
    for (auto expected_story_id : expected_story_ids) {
      CHECK(expected_story_id != StoryId());
      all_story_ids.insert(expected_story_id);
    }
    for (auto story_id : story_ids) {
      if (all_story_ids.erase(story_id) == 0) {
        LOG(ERROR) << "Receive " << story_id << " in " << owner_dialog_id << ", but didn't request it";
      }
    }
    for (auto story_id : all_story_ids) {
      on_delete_story({owner_dialog_id, story_id});
    }
  }
  return {total_count, std::move(story_ids)};
}

DialogId StoryManager::on_get_user_stories(DialogId owner_dialog_id,
                                           telegram_api::object_ptr<telegram_api::userStories> &&user_stories,
                                           Promise<Unit> &&promise) {
  if (user_stories == nullptr) {
    if (owner_dialog_id.is_valid()) {
      LOG(INFO) << "Receive no stories in " << owner_dialog_id;
      on_update_active_stories(owner_dialog_id, StoryId(), {}, std::move(promise), "on_get_user_stories");
    } else {
      promise.set_value(Unit());
    }
    return owner_dialog_id;
  }

  DialogId story_dialog_id(UserId(user_stories->user_id_));
  if (owner_dialog_id.is_valid() && owner_dialog_id != story_dialog_id) {
    LOG(ERROR) << "Receive stories from " << story_dialog_id << " instead of " << owner_dialog_id;
    on_update_active_stories(owner_dialog_id, StoryId(), {}, std::move(promise), "on_get_user_stories 2");
    return owner_dialog_id;
  }
  if (!story_dialog_id.is_valid()) {
    LOG(ERROR) << "Receive stories in " << story_dialog_id;
    promise.set_value(Unit());
    return owner_dialog_id;
  }
  owner_dialog_id = story_dialog_id;

  StoryId max_read_story_id(user_stories->max_read_id_);
  if (!max_read_story_id.is_server() && max_read_story_id != StoryId()) {
    LOG(ERROR) << "Receive max read " << max_read_story_id;
    max_read_story_id = StoryId();
  }

  vector<StoryId> story_ids;
  for (auto &story : user_stories->stories_) {
    switch (story->get_id()) {
      case telegram_api::storyItemDeleted::ID:
        on_get_deleted_story(owner_dialog_id, telegram_api::move_object_as<telegram_api::storyItemDeleted>(story));
        break;
      case telegram_api::storyItemSkipped::ID:
        story_ids.push_back(
            on_get_skipped_story(owner_dialog_id, telegram_api::move_object_as<telegram_api::storyItemSkipped>(story)));
        break;
      case telegram_api::storyItem::ID:
        story_ids.push_back(
            on_get_new_story(owner_dialog_id, telegram_api::move_object_as<telegram_api::storyItem>(story)));
        break;
      default:
        UNREACHABLE();
    }
  }

  on_update_active_stories(story_dialog_id, max_read_story_id, std::move(story_ids), std::move(promise),
                           "on_get_user_stories 3");
  return story_dialog_id;
}

void StoryManager::on_update_active_stories(DialogId owner_dialog_id, StoryId max_read_story_id,
                                            vector<StoryId> &&story_ids, Promise<Unit> &&promise, const char *source,
                                            bool from_database) {
  CHECK(owner_dialog_id.is_valid());
  if (td::remove_if(story_ids, [&](StoryId story_id) {
        if (!story_id.is_server()) {
          return true;
        }
        if (!is_active_story(get_story({owner_dialog_id, story_id}))) {
          LOG(INFO) << "Receive expired " << story_id << " in " << owner_dialog_id << " from " << source;
          return true;
        }
        return false;
      })) {
    from_database = false;
  }
  if (story_ids.empty() || max_read_story_id.get() < story_ids[0].get()) {
    max_read_story_id = StoryId();
  }

  LOG(INFO) << "Update active stories in " << owner_dialog_id << " to " << story_ids << " with max read "
            << max_read_story_id << " from " << source;

  if (story_ids.empty()) {
    if (owner_dialog_id.get_type() == DialogType::User) {
      td_->contacts_manager_->on_update_user_has_stories(owner_dialog_id.get_user_id(), false, StoryId(), StoryId());
    }
    auto *active_stories = get_active_stories(owner_dialog_id);
    if (active_stories != nullptr) {
      LOG(INFO) << "Delete active stories for " << owner_dialog_id;
      if (active_stories->story_list_id_.is_valid()) {
        delete_active_stories_from_story_list(owner_dialog_id, active_stories);
        auto &story_list = get_story_list(active_stories->story_list_id_);
        if (!from_database && story_list.is_reloaded_server_total_count_ &&
            story_list.server_total_count_ > static_cast<int32>(story_list.ordered_stories_.size())) {
          story_list.server_total_count_--;
          save_story_list(active_stories->story_list_id_, story_list.state_, story_list.server_total_count_,
                          story_list.server_has_more_);
        }
        update_story_list_sent_total_count(active_stories->story_list_id_, story_list);
      }
      active_stories_.erase(owner_dialog_id);
      send_update_chat_active_stories(owner_dialog_id, nullptr);
    } else {
      max_read_story_ids_.erase(owner_dialog_id);
    }
    if (!from_database) {
      save_active_stories(owner_dialog_id, nullptr, std::move(promise), source);
    }
    failed_to_load_active_stories_.insert(owner_dialog_id);
    return;
  }
  failed_to_load_active_stories_.erase(owner_dialog_id);

  auto &active_stories = active_stories_[owner_dialog_id];
  if (active_stories == nullptr) {
    LOG(INFO) << "Create active stories for " << owner_dialog_id << " from " << source;
    active_stories = make_unique<ActiveStories>();
    auto old_max_read_story_id = max_read_story_ids_.get(owner_dialog_id);
    if (old_max_read_story_id != StoryId()) {
      max_read_story_ids_.erase(owner_dialog_id);
      if (old_max_read_story_id.get() > max_read_story_id.get() && old_max_read_story_id.get() >= story_ids[0].get()) {
        max_read_story_id = old_max_read_story_id;
      }
    }
  }
  if (owner_dialog_id.get_type() == DialogType::User) {
    td_->contacts_manager_->on_update_user_has_stories(owner_dialog_id.get_user_id(), true, story_ids.back(),
                                                       max_read_story_id);
  }
  bool need_save_to_database = false;
  if (active_stories->max_read_story_id_ != max_read_story_id || active_stories->story_ids_ != story_ids) {
    need_save_to_database = true;
    active_stories->max_read_story_id_ = max_read_story_id;
    active_stories->story_ids_ = std::move(story_ids);
    update_active_stories_order(owner_dialog_id, active_stories.get(), &need_save_to_database);
    send_update_chat_active_stories(owner_dialog_id, active_stories.get());
  } else if (update_active_stories_order(owner_dialog_id, active_stories.get(), &need_save_to_database)) {
    send_update_chat_active_stories(owner_dialog_id, active_stories.get());
  }
  if (need_save_to_database && !from_database) {
    save_active_stories(owner_dialog_id, active_stories.get(), std::move(promise), source);
  } else {
    promise.set_value(Unit());
  }
}

bool StoryManager::update_active_stories_order(DialogId owner_dialog_id, ActiveStories *active_stories,
                                               bool *need_save_to_database) {
  if (td_->auth_manager_->is_bot()) {
    return false;
  }

  CHECK(active_stories != nullptr);
  CHECK(!active_stories->story_ids_.empty());
  CHECK(owner_dialog_id.is_valid());

  auto last_story_id = active_stories->story_ids_.back();
  const Story *last_story = get_story({owner_dialog_id, last_story_id});
  CHECK(last_story != nullptr);

  int64 new_private_order = 0;
  new_private_order += last_story->date_;
  if (owner_dialog_id.get_type() == DialogType::User &&
      td_->contacts_manager_->is_user_premium(owner_dialog_id.get_user_id())) {
    new_private_order += static_cast<int64>(1) << 33;
  }
  if (owner_dialog_id == get_changelog_story_dialog_id()) {
    new_private_order += static_cast<int64>(1) << 34;
  }
  if (active_stories->max_read_story_id_.get() < last_story_id.get()) {
    new_private_order += static_cast<int64>(1) << 35;
  }
  if (owner_dialog_id == DialogId(td_->contacts_manager_->get_my_id())) {
    new_private_order += static_cast<int64>(1) << 36;
  }
  CHECK(new_private_order != 0);

  StoryListId story_list_id = get_dialog_story_list_id(owner_dialog_id);
  LOG(INFO) << "Update order of active stories of " << owner_dialog_id << " in " << story_list_id << " from "
            << active_stories->private_order_ << '/' << active_stories->public_order_ << " to " << new_private_order;

  int64 new_public_order = 0;
  if (story_list_id.is_valid()) {
    auto &story_list = get_story_list(story_list_id);
    if (DialogDate(new_private_order, owner_dialog_id) <= story_list.list_last_story_date_) {
      new_public_order = new_private_order;
    }

    if (active_stories->private_order_ != new_private_order || active_stories->story_list_id_ != story_list_id) {
      delete_active_stories_from_story_list(owner_dialog_id, active_stories);
      bool is_inserted = story_list.ordered_stories_.insert({new_private_order, owner_dialog_id}).second;
      CHECK(is_inserted);

      if (active_stories->story_list_id_ != story_list_id && active_stories->story_list_id_.is_valid()) {
        update_story_list_sent_total_count(active_stories->story_list_id_);
      }
      update_story_list_sent_total_count(story_list_id, story_list);
    }
  } else if (active_stories->story_list_id_.is_valid()) {
    delete_active_stories_from_story_list(owner_dialog_id, active_stories);
    update_story_list_sent_total_count(active_stories->story_list_id_);
  }

  if (active_stories->private_order_ != new_private_order || active_stories->public_order_ != new_public_order ||
      active_stories->story_list_id_ != story_list_id) {
    LOG(INFO) << "Update order of active stories of " << owner_dialog_id << " to " << new_private_order << '/'
              << new_public_order << " in list " << story_list_id;
    if (active_stories->private_order_ != new_private_order || active_stories->story_list_id_ != story_list_id) {
      *need_save_to_database = true;
    }
    active_stories->private_order_ = new_private_order;
    if (active_stories->public_order_ != new_public_order || active_stories->story_list_id_ != story_list_id) {
      if (active_stories->story_list_id_ != story_list_id) {
        if (active_stories->story_list_id_.is_valid() && active_stories->public_order_ != 0) {
          active_stories->public_order_ = 0;
          send_update_chat_active_stories(owner_dialog_id, active_stories);
        }
        active_stories->story_list_id_ = story_list_id;
      }
      active_stories->public_order_ = new_public_order;
      return true;
    }
  }

  return false;
}

void StoryManager::delete_active_stories_from_story_list(DialogId owner_dialog_id,
                                                         const ActiveStories *active_stories) {
  if (!active_stories->story_list_id_.is_valid()) {
    return;
  }
  auto &story_list = get_story_list(active_stories->story_list_id_);
  bool is_deleted = story_list.ordered_stories_.erase({active_stories->private_order_, owner_dialog_id}) > 0;
  CHECK(is_deleted);
}

void StoryManager::send_update_story(StoryFullId story_full_id, const Story *story) {
  auto story_object = get_story_object(story_full_id, story);
  CHECK(story_object != nullptr);
  send_closure(G()->td(), &Td::send_update, td_api::make_object<td_api::updateStory>(std::move(story_object)));
}

td_api::object_ptr<td_api::updateChatActiveStories> StoryManager::get_update_chat_active_stories(
    DialogId owner_dialog_id, const ActiveStories *active_stories) const {
  return td_api::make_object<td_api::updateChatActiveStories>(
      get_chat_active_stories_object(owner_dialog_id, active_stories));
}

void StoryManager::send_update_chat_active_stories(DialogId owner_dialog_id,
                                                   const ActiveStories *active_stories) const {
  send_closure(G()->td(), &Td::send_update, get_update_chat_active_stories(owner_dialog_id, active_stories));
}

void StoryManager::save_active_stories(DialogId owner_dialog_id, const ActiveStories *active_stories,
                                       Promise<Unit> &&promise, const char *source) const {
  if (!G()->use_message_database()) {
    return promise.set_value(Unit());
  }
  if (active_stories == nullptr) {
    LOG(INFO) << "Delete active stories of " << owner_dialog_id << " from database from " << source;
    G()->td_db()->get_story_db_async()->delete_active_stories(owner_dialog_id, std::move(promise));
  } else {
    LOG(INFO) << "Add active stories of " << owner_dialog_id << " to database from " << source;
    auto order = active_stories->story_list_id_.is_valid() ? active_stories->private_order_ : 0;
    SavedActiveStories saved_active_stories;
    saved_active_stories.max_read_story_id_ = active_stories->max_read_story_id_;
    for (auto story_id : active_stories->story_ids_) {
      auto story_info = get_story_info({owner_dialog_id, story_id});
      if (story_info.story_id_.is_valid()) {
        saved_active_stories.story_infos_.push_back(std::move(story_info));
      }
    }
    G()->td_db()->get_story_db_async()->add_active_stories(owner_dialog_id, active_stories->story_list_id_, order,
                                                           log_event_store(saved_active_stories), std::move(promise));
  }
}

bool StoryManager::on_update_read_stories(DialogId owner_dialog_id, StoryId max_read_story_id) {
  if (!td_->messages_manager_->have_dialog_info_force(owner_dialog_id)) {
    return false;
  }
  auto active_stories = get_active_stories_force(owner_dialog_id, "on_update_read_stories");
  if (active_stories == nullptr) {
    LOG(INFO) << "Can't find active stories in " << owner_dialog_id;
    auto old_max_read_story_id = max_read_story_ids_.get(owner_dialog_id);
    if (max_read_story_id.get() > old_max_read_story_id.get()) {
      LOG(INFO) << "Set max read story identifier in " << owner_dialog_id << " to " << max_read_story_id;
      max_read_story_ids_.set(owner_dialog_id, max_read_story_id);
      if (owner_dialog_id.get_type() == DialogType::User) {
        auto user_id = owner_dialog_id.get_user_id();
        if (td_->contacts_manager_->have_user(user_id)) {
          td_->contacts_manager_->on_update_user_max_read_story_id(user_id, max_read_story_id);
        }
      }
      return true;
    }
  } else if (max_read_story_id.get() > active_stories->max_read_story_id_.get()) {
    LOG(INFO) << "Update max read story identifier in " << owner_dialog_id << " with stories "
              << active_stories->story_ids_ << " from " << active_stories->max_read_story_id_ << " to "
              << max_read_story_id;
    auto story_ids = active_stories->story_ids_;
    on_update_active_stories(owner_dialog_id, max_read_story_id, std::move(story_ids), Promise<Unit>(),
                             "on_update_read_stories");
    return true;
  }
  return false;
}

DialogId StoryManager::get_changelog_story_dialog_id() const {
  return DialogId(UserId(td_->option_manager_->get_option_integer(
      "stories_changelog_user_id", ContactsManager::get_service_notifications_user_id().get())));
}

bool StoryManager::is_subscribed_to_dialog_stories(DialogId owner_dialog_id) const {
  if (owner_dialog_id == get_changelog_story_dialog_id()) {
    return true;
  }
  switch (owner_dialog_id.get_type()) {
    case DialogType::User:
      if (owner_dialog_id == DialogId(td_->contacts_manager_->get_my_id())) {
        return true;
      }
      return td_->contacts_manager_->is_user_contact(owner_dialog_id.get_user_id());
    case DialogType::Chat:
    case DialogType::Channel:
    case DialogType::SecretChat:
    case DialogType::None:
    default:
      return false;
  }
}

StoryListId StoryManager::get_dialog_story_list_id(DialogId owner_dialog_id) const {
  if (!is_subscribed_to_dialog_stories(owner_dialog_id)) {
    return StoryListId();
  }
  switch (owner_dialog_id.get_type()) {
    case DialogType::User:
      if (owner_dialog_id != DialogId(td_->contacts_manager_->get_my_id()) &&
          td_->contacts_manager_->get_user_stories_hidden(owner_dialog_id.get_user_id())) {
        return StoryListId::archive();
      }
      return StoryListId::main();
    case DialogType::Chat:
    case DialogType::Channel:
    case DialogType::SecretChat:
    case DialogType::None:
    default:
      return StoryListId::archive();
  }
}

void StoryManager::on_dialog_active_stories_order_updated(DialogId owner_dialog_id, const char *source) {
  LOG(INFO) << "Update order of active stories in " << owner_dialog_id << " from " << source;
  // called from update_user, must not create the dialog and hence must not load active stories
  auto active_stories = get_active_stories_editable(owner_dialog_id);
  bool need_save_to_database = false;
  if (active_stories != nullptr &&
      update_active_stories_order(owner_dialog_id, active_stories, &need_save_to_database)) {
    send_update_chat_active_stories(owner_dialog_id, active_stories);
  }
  if (need_save_to_database) {
    save_active_stories(owner_dialog_id, active_stories, Promise<Unit>(), "on_dialog_active_stories_order_updated");
  }
}

void StoryManager::on_get_story_views(const vector<StoryId> &story_ids,
                                      telegram_api::object_ptr<telegram_api::stories_storyViews> &&story_views) {
  schedule_interaction_info_update();
  td_->contacts_manager_->on_get_users(std::move(story_views->users_), "on_get_story_views");
  if (story_ids.size() != story_views->views_.size()) {
    LOG(ERROR) << "Receive invalid views for " << story_ids << ": " << to_string(story_views);
    return;
  }
  DialogId owner_dialog_id(td_->contacts_manager_->get_my_id());
  for (size_t i = 0; i < story_ids.size(); i++) {
    auto story_id = story_ids[i];
    CHECK(story_id.is_server());

    StoryFullId story_full_id{owner_dialog_id, story_id};
    Story *story = get_story_editable(story_full_id);
    if (story == nullptr || story->content_ == nullptr) {
      continue;
    }

    StoryInteractionInfo interaction_info(td_, std::move(story_views->views_[i]));
    CHECK(!interaction_info.is_empty());
    if (story->interaction_info_ != interaction_info) {
      story->interaction_info_ = std::move(interaction_info);
      on_story_changed(story_full_id, story, true, true);
    }
  }
}

FileSourceId StoryManager::get_story_file_source_id(StoryFullId story_full_id) {
  if (td_->auth_manager_->is_bot()) {
    return FileSourceId();
  }

  if (!story_full_id.is_valid()) {
    return FileSourceId();
  }

  auto &file_source_id = story_full_id_to_file_source_id_[story_full_id];
  if (!file_source_id.is_valid()) {
    file_source_id = td_->file_reference_manager_->create_story_file_source(story_full_id);
  }
  return file_source_id;
}

void StoryManager::reload_story(StoryFullId story_full_id, Promise<Unit> &&promise, const char *source) {
  if (deleted_story_full_ids_.count(story_full_id) > 0) {
    return promise.set_value(Unit());
  }
  double last_reloaded_at = inaccessible_story_full_ids_.get(story_full_id);
  if (last_reloaded_at >= Time::now() - OPENED_STORY_POLL_PERIOD / 2 && last_reloaded_at > 0.0) {
    return promise.set_value(Unit());
  }

  LOG(INFO) << "Reload " << story_full_id << " from " << source;
  auto dialog_id = story_full_id.get_dialog_id();
  if (dialog_id.get_type() != DialogType::User) {
    return promise.set_error(Status::Error(400, "Unsupported story owner"));
  }
  auto story_id = story_full_id.get_story_id();
  if (!story_id.is_server()) {
    return promise.set_error(Status::Error(400, "Invalid story identifier"));
  }

  auto &queries = reload_story_queries_[story_full_id];
  if (!queries.empty() && !promise) {
    return;
  }
  queries.push_back(std::move(promise));
  if (queries.size() != 1) {
    return;
  }

  auto query_promise =
      PromiseCreator::lambda([actor_id = actor_id(this), story_full_id](Result<Unit> &&result) mutable {
        send_closure(actor_id, &StoryManager::on_reload_story, story_full_id, std::move(result));
      });
  td_->create_handler<GetStoriesByIDQuery>(std::move(query_promise))->send(dialog_id.get_user_id(), {story_id});
}

void StoryManager::on_reload_story(StoryFullId story_full_id, Result<Unit> &&result) {
  if (G()->close_flag()) {
    return;
  }
  auto it = reload_story_queries_.find(story_full_id);
  CHECK(it != reload_story_queries_.end());
  CHECK(!it->second.empty());
  auto promises = std::move(it->second);
  reload_story_queries_.erase(it);

  if (result.is_ok()) {
    set_promises(promises);
  } else {
    fail_promises(promises, result.move_as_error());
  }
}

void StoryManager::get_story(DialogId owner_dialog_id, StoryId story_id, bool only_local,
                             Promise<td_api::object_ptr<td_api::story>> &&promise) {
  if (!td_->messages_manager_->have_dialog_force(owner_dialog_id, "get_story")) {
    return promise.set_error(Status::Error(400, "Story sender not found"));
  }
  if (!td_->messages_manager_->have_input_peer(owner_dialog_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(400, "Can't access the story sender"));
  }
  if (!story_id.is_server()) {
    return promise.set_error(Status::Error(400, "Invalid story identifier specified"));
  }
  if (owner_dialog_id.get_type() != DialogType::User) {
    return promise.set_value(nullptr);
  }

  StoryFullId story_full_id{owner_dialog_id, story_id};
  const Story *story = get_story_force(story_full_id, "get_story");
  if (story != nullptr && story->content_ != nullptr) {
    if (!story->is_update_sent_) {
      send_update_story(story_full_id, story);
    }
    return promise.set_value(get_story_object(story_full_id, story));
  }
  if (only_local) {
    return promise.set_value(nullptr);
  }

  auto query_promise = PromiseCreator::lambda(
      [actor_id = actor_id(this), story_full_id, promise = std::move(promise)](Result<Unit> &&result) mutable {
        send_closure(actor_id, &StoryManager::do_get_story, story_full_id, std::move(result), std::move(promise));
      });
  reload_story(story_full_id, std::move(query_promise), "get_story");
}

void StoryManager::do_get_story(StoryFullId story_full_id, Result<Unit> &&result,
                                Promise<td_api::object_ptr<td_api::story>> &&promise) {
  G()->ignore_result_if_closing(result);
  if (result.is_error()) {
    return promise.set_error(result.move_as_error());
  }
  const Story *story = get_story(story_full_id);
  if (story != nullptr && story->content_ != nullptr && !story->is_update_sent_) {
    send_update_story(story_full_id, story);
  }
  promise.set_value(get_story_object(story_full_id, story));
}

void StoryManager::send_story(td_api::object_ptr<td_api::InputStoryContent> &&input_story_content,
                              td_api::object_ptr<td_api::formattedText> &&input_caption,
                              td_api::object_ptr<td_api::StoryPrivacySettings> &&settings, int32 active_period,
                              bool is_pinned, bool protect_content,
                              Promise<td_api::object_ptr<td_api::story>> &&promise) {
  bool is_bot = td_->auth_manager_->is_bot();
  DialogId dialog_id(td_->contacts_manager_->get_my_id());
  TRY_RESULT_PROMISE(promise, content, get_input_story_content(td_, std::move(input_story_content), dialog_id));
  TRY_RESULT_PROMISE(promise, caption,
                     get_formatted_text(td_, DialogId(), std::move(input_caption), is_bot, true, false, false));
  TRY_RESULT_PROMISE(promise, privacy_rules,
                     UserPrivacySettingRules::get_user_privacy_setting_rules(td_, std::move(settings)));
  if (active_period != 86400 && !(G()->is_test_dc() && (active_period == 60 || active_period == 300))) {
    bool is_premium = td_->option_manager_->get_option_boolean("is_premium");
    if (!is_premium ||
        !td::contains(vector<int32>{6 * 3600, 12 * 3600, 2 * 86400, 3 * 86400, 7 * 86400}, active_period)) {
      return promise.set_error(Status::Error(400, "Invalid story active period specified"));
    }
  }

  td_->messages_manager_->force_create_dialog(dialog_id, "send_story");

  auto story = make_unique<Story>();
  story->date_ = G()->unix_time();
  story->expire_date_ = story->date_ + active_period;
  story->is_pinned_ = is_pinned;
  story->noforwards_ = protect_content;
  story->privacy_rules_ = std::move(privacy_rules);
  story->content_ = dup_story_content(td_, content.get());
  story->caption_ = std::move(caption);

  int64 random_id;
  do {
    random_id = Random::secure_int64();
  } while (random_id == 0);

  auto story_ptr = story.get();

  auto pending_story =
      td::make_unique<PendingStory>(dialog_id, StoryId(), ++send_story_count_, random_id, std::move(story));
  pending_story->log_event_id_ = save_send_story_log_event(pending_story.get());

  yet_unsent_stories_.insert(pending_story->send_story_num_);

  do_send_story(std::move(pending_story), {});

  promise.set_value(get_story_object({dialog_id, StoryId()}, story_ptr));
}

class StoryManager::SendStoryLogEvent {
 public:
  const PendingStory *pending_story_in_;
  unique_ptr<PendingStory> pending_story_out_;

  SendStoryLogEvent() : pending_story_in_(nullptr) {
  }

  explicit SendStoryLogEvent(const PendingStory *pending_story) : pending_story_in_(pending_story) {
  }

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(*pending_story_in_, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(pending_story_out_, parser);
  }
};

int64 StoryManager::save_send_story_log_event(const PendingStory *pending_story) {
  if (!G()->use_message_database()) {
    return 0;
  }

  return binlog_add(G()->td_db()->get_binlog(), LogEvent::HandlerType::SendStory,
                    get_log_event_storer(SendStoryLogEvent(pending_story)));
}

void StoryManager::do_send_story(unique_ptr<PendingStory> &&pending_story, vector<int> bad_parts) {
  CHECK(pending_story != nullptr);
  CHECK(pending_story->story_ != nullptr);
  auto content = pending_story->story_->content_.get();
  CHECK(content != nullptr);
  auto upload_order = pending_story->send_story_num_;

  FileId file_id = get_story_content_any_file_id(td_, content);
  CHECK(file_id.is_valid());

  LOG(INFO) << "Ask to upload file " << file_id << " with bad parts " << bad_parts;
  bool is_inserted = being_uploaded_files_.emplace(file_id, std::move(pending_story)).second;
  CHECK(is_inserted);
  // need to call resume_upload synchronously to make upload process consistent with being_uploaded_files_
  // and to send is_uploading_active == true in response
  td_->file_manager_->resume_upload(file_id, std::move(bad_parts), upload_media_callback_, 1, upload_order);
}

void StoryManager::on_upload_story(FileId file_id, telegram_api::object_ptr<telegram_api::InputFile> input_file) {
  if (G()->close_flag()) {
    return;
  }

  LOG(INFO) << "File " << file_id << " has been uploaded";

  auto it = being_uploaded_files_.find(file_id);
  if (it == being_uploaded_files_.end()) {
    // callback may be called just before the file upload was canceled
    return;
  }

  auto pending_story = std::move(it->second);

  being_uploaded_files_.erase(it);

  FileView file_view = td_->file_manager_->get_file_view(file_id);
  CHECK(!file_view.is_encrypted());
  if (input_file == nullptr && file_view.has_remote_location()) {
    if (file_view.main_remote_location().is_web()) {
      delete_pending_story(file_id, std::move(pending_story), Status::Error(400, "Can't use web photo as a story"));
      return;
    }
    if (pending_story->was_reuploaded_) {
      delete_pending_story(file_id, std::move(pending_story), Status::Error(500, "Failed to reupload story"));
      return;
    }
    pending_story->was_reuploaded_ = true;

    // delete file reference and forcely reupload the file
    td_->file_manager_->delete_file_reference(file_id, file_view.main_remote_location().get_file_reference());
    do_send_story(std::move(pending_story), {-1});
    return;
  }
  CHECK(input_file != nullptr);

  bool is_edit = pending_story->story_id_.is_server();
  if (is_edit) {
    do_edit_story(file_id, std::move(pending_story), std::move(input_file));
  } else {
    auto send_story_num = pending_story->send_story_num_;
    LOG(INFO) << "Story " << send_story_num << " is ready to be sent";
    ready_to_send_stories_.emplace(
        send_story_num, td::make_unique<ReadyToSendStory>(file_id, std::move(pending_story), std::move(input_file)));
    try_send_story();
  }
}

void StoryManager::on_upload_story_error(FileId file_id, Status status) {
  if (G()->close_flag()) {
    // do not fail upload if closing
    return;
  }

  LOG(INFO) << "File " << file_id << " has upload error " << status;

  auto it = being_uploaded_files_.find(file_id);
  if (it == being_uploaded_files_.end()) {
    // callback may be called just before the file upload was canceled
    return;
  }

  auto pending_story = std::move(it->second);

  being_uploaded_files_.erase(it);

  delete_pending_story(file_id, std::move(pending_story), std::move(status));
}

void StoryManager::try_send_story() {
  if (yet_unsent_stories_.empty()) {
    LOG(INFO) << "There is no more stories to send";
    return;
  }
  auto send_story_num = *yet_unsent_stories_.begin();
  auto it = ready_to_send_stories_.find(send_story_num);
  if (it == ready_to_send_stories_.end()) {
    LOG(INFO) << "Story " << send_story_num << " isn't ready to be sent or is being sent";
    return;
  }
  auto ready_to_send_story = std::move(it->second);
  ready_to_send_stories_.erase(it);

  td_->create_handler<SendStoryQuery>()->send(ready_to_send_story->file_id_,
                                              std::move(ready_to_send_story->pending_story_),
                                              std::move(ready_to_send_story->input_file_));
}

void StoryManager::on_send_story_file_parts_missing(unique_ptr<PendingStory> &&pending_story, vector<int> &&bad_parts) {
  do_send_story(std::move(pending_story), std::move(bad_parts));
}

class StoryManager::EditStoryLogEvent {
 public:
  const PendingStory *pending_story_in_;
  unique_ptr<PendingStory> pending_story_out_;
  bool edit_caption_;
  FormattedText caption_;

  EditStoryLogEvent() : pending_story_in_(nullptr), edit_caption_(false) {
  }

  EditStoryLogEvent(const PendingStory *pending_story, bool edit_caption, const FormattedText &caption)
      : pending_story_in_(pending_story), edit_caption_(edit_caption), caption_(caption) {
  }

  template <class StorerT>
  void store(StorerT &storer) const {
    bool has_caption = edit_caption_ && !caption_.text.empty();
    BEGIN_STORE_FLAGS();
    STORE_FLAG(edit_caption_);
    STORE_FLAG(has_caption);
    END_STORE_FLAGS();
    td::store(*pending_story_in_, storer);
    if (has_caption) {
      td::store(caption_, storer);
    }
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    bool has_caption;
    BEGIN_PARSE_FLAGS();
    PARSE_FLAG(edit_caption_);
    PARSE_FLAG(has_caption);
    END_PARSE_FLAGS();
    td::parse(pending_story_out_, parser);
    if (has_caption) {
      td::parse(caption_, parser);
    }
  }
};

void StoryManager::edit_story(StoryId story_id, td_api::object_ptr<td_api::InputStoryContent> &&input_story_content,
                              td_api::object_ptr<td_api::formattedText> &&input_caption, Promise<Unit> &&promise) {
  DialogId dialog_id(td_->contacts_manager_->get_my_id());
  StoryFullId story_full_id{dialog_id, story_id};
  const Story *story = get_story(story_full_id);
  if (story == nullptr || story->content_ == nullptr) {
    return promise.set_error(Status::Error(400, "Story not found"));
  }
  if (!story_id.is_server()) {
    return promise.set_error(Status::Error(400, "Story can't be edited"));
  }

  bool is_bot = td_->auth_manager_->is_bot();
  unique_ptr<StoryContent> content;
  bool is_caption_edited = input_caption != nullptr;
  FormattedText caption;
  if (input_story_content != nullptr) {
    TRY_RESULT_PROMISE_ASSIGN(promise, content,
                              get_input_story_content(td_, std::move(input_story_content), dialog_id));
  }
  if (is_caption_edited) {
    TRY_RESULT_PROMISE_ASSIGN(
        promise, caption, get_formatted_text(td_, DialogId(), std::move(input_caption), is_bot, true, false, false));
    auto *current_caption = &story->caption_;
    auto it = being_edited_stories_.find(story_full_id);
    if (it != being_edited_stories_.end() && it->second->edit_caption_) {
      current_caption = &it->second->caption_;
    }
    if (*current_caption == caption) {
      is_caption_edited = false;
    }
  }
  if (content == nullptr && !is_caption_edited) {
    return promise.set_value(Unit());
  }

  auto &edited_story = being_edited_stories_[story_full_id];
  if (edited_story == nullptr) {
    edited_story = make_unique<BeingEditedStory>();
  }
  auto &edit_generation = edit_generations_[story_full_id];
  if (content != nullptr) {
    edited_story->content_ = std::move(content);
    edit_generation++;
  }
  if (is_caption_edited) {
    edited_story->caption_ = std::move(caption);
    edited_story->edit_caption_ = true;
    edit_generation++;
  }
  edited_story->promises_.push_back(std::move(promise));

  auto new_story = make_unique<Story>();
  new_story->content_ = dup_story_content(td_, edited_story->content_.get());

  auto pending_story =
      td::make_unique<PendingStory>(dialog_id, story_id, std::numeric_limits<uint32>::max() - (++send_story_count_),
                                    edit_generation, std::move(new_story));
  if (G()->use_message_database()) {
    EditStoryLogEvent log_event(pending_story.get(), edited_story->edit_caption_, edited_story->caption_);
    auto storer = get_log_event_storer(log_event);
    auto &cur_log_event_id = edited_story->log_event_id_;
    if (cur_log_event_id == 0) {
      cur_log_event_id = binlog_add(G()->td_db()->get_binlog(), LogEvent::HandlerType::EditStory, storer);
      LOG(INFO) << "Add edit story log event " << cur_log_event_id;
    } else {
      auto new_log_event_id =
          binlog_rewrite(G()->td_db()->get_binlog(), cur_log_event_id, LogEvent::HandlerType::EditStory, storer);
      LOG(INFO) << "Rewrite edit story log event " << cur_log_event_id << " with " << new_log_event_id;
    }
  }

  on_story_changed(story_full_id, story, true, true);

  if (edited_story->content_ == nullptr) {
    return do_edit_story(FileId(), std::move(pending_story), nullptr);
  }

  do_send_story(std::move(pending_story), {});
}

void StoryManager::do_edit_story(FileId file_id, unique_ptr<PendingStory> &&pending_story,
                                 telegram_api::object_ptr<telegram_api::InputFile> input_file) {
  StoryFullId story_full_id{pending_story->dialog_id_, pending_story->story_id_};
  const Story *story = get_story(story_full_id);
  auto it = being_edited_stories_.find(story_full_id);
  if (story == nullptr || it == being_edited_stories_.end() ||
      edit_generations_[story_full_id] != pending_story->random_id_) {
    LOG(INFO) << "Skip outdated edit of " << story_full_id;
    if (file_id.is_valid()) {
      td_->file_manager_->cancel_upload(file_id);
    }
    return;
  }
  CHECK(story->content_ != nullptr);
  td_->create_handler<EditStoryQuery>()->send(file_id, std::move(pending_story), std::move(input_file),
                                              it->second.get());
}

void StoryManager::delete_pending_story(FileId file_id, unique_ptr<PendingStory> &&pending_story, Status status) {
  if (file_id.is_valid()) {
    td_->file_manager_->delete_partial_remote_location(file_id);
  }

  CHECK(pending_story != nullptr);
  bool is_edit = pending_story->story_id_.is_server();
  if (is_edit) {
    StoryFullId story_full_id{pending_story->dialog_id_, pending_story->story_id_};
    const Story *story = get_story(story_full_id);
    auto it = being_edited_stories_.find(story_full_id);
    if (story == nullptr || it == being_edited_stories_.end() ||
        edit_generations_[story_full_id] != pending_story->random_id_) {
      LOG(INFO) << "Ignore outdated edit of " << story_full_id;
      return;
    }
    CHECK(story->content_ != nullptr);
    auto promises = std::move(it->second->promises_);
    auto log_event_id = it->second->log_event_id_;
    if (log_event_id != 0) {
      binlog_erase(G()->td_db()->get_binlog(), log_event_id);
    }
    being_edited_stories_.erase(it);

    on_story_changed(story_full_id, story, true, true);

    if (status.is_ok()) {
      set_promises(promises);
    } else {
      fail_promises(promises, std::move(status));
    }
    CHECK(pending_story->log_event_id_ == 0);
  } else {
    LOG(INFO) << "Finish sending of story " << pending_story->send_story_num_;
    yet_unsent_stories_.erase(pending_story->send_story_num_);
    try_send_story();

    if (pending_story->log_event_id_ != 0) {
      binlog_erase(G()->td_db()->get_binlog(), pending_story->log_event_id_);
    }
  }
}

void StoryManager::set_story_privacy_settings(StoryId story_id,
                                              td_api::object_ptr<td_api::StoryPrivacySettings> &&settings,
                                              Promise<Unit> &&promise) {
  DialogId dialog_id(td_->contacts_manager_->get_my_id());
  const Story *story = get_story({dialog_id, story_id});
  if (story == nullptr || story->content_ == nullptr) {
    return promise.set_error(Status::Error(400, "Story not found"));
  }
  TRY_RESULT_PROMISE(promise, privacy_rules,
                     UserPrivacySettingRules::get_user_privacy_setting_rules(td_, std::move(settings)));
  td_->create_handler<EditStoryPrivacyQuery>(std::move(promise))->send(dialog_id, story_id, std::move(privacy_rules));
}

void StoryManager::toggle_story_is_pinned(StoryId story_id, bool is_pinned, Promise<Unit> &&promise) {
  DialogId dialog_id(td_->contacts_manager_->get_my_id());
  const Story *story = get_story({dialog_id, story_id});
  if (story == nullptr || story->content_ == nullptr) {
    return promise.set_error(Status::Error(400, "Story not found"));
  }
  auto query_promise = PromiseCreator::lambda(
      [actor_id = actor_id(this), story_id, is_pinned, promise = std::move(promise)](Result<Unit> &&result) mutable {
        if (result.is_error()) {
          return promise.set_error(result.move_as_error());
        }
        send_closure(actor_id, &StoryManager::on_toggle_story_is_pinned, story_id, is_pinned, std::move(promise));
      });
  td_->create_handler<ToggleStoryPinnedQuery>(std::move(query_promise))->send(dialog_id, story_id, is_pinned);
}

void StoryManager::on_toggle_story_is_pinned(StoryId story_id, bool is_pinned, Promise<Unit> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());
  DialogId dialog_id(td_->contacts_manager_->get_my_id());
  Story *story = get_story_editable({dialog_id, story_id});
  if (story != nullptr) {
    CHECK(story->content_ != nullptr);
    story->is_pinned_ = is_pinned;
    on_story_changed({dialog_id, story_id}, story, true, true);
  }
  promise.set_value(Unit());
}

void StoryManager::delete_story(StoryId story_id, Promise<Unit> &&promise) {
  DialogId owner_dialog_id(td_->contacts_manager_->get_my_id());
  StoryFullId story_full_id{owner_dialog_id, story_id};
  const Story *story = get_story(story_full_id);
  if (story == nullptr) {
    return promise.set_error(Status::Error(400, "Story not found"));
  }
  if (!story_id.is_server()) {
    return promise.set_error(Status::Error(400, "Invalid story identifier"));
  }

  delete_story_on_server(story_full_id, 0, std::move(promise));
}

class StoryManager::DeleteStoryOnServerLogEvent {
 public:
  StoryFullId story_full_id_;

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(story_full_id_, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(story_full_id_, parser);
  }
};

uint64 StoryManager::save_delete_story_on_server_log_event(StoryFullId story_full_id) {
  DeleteStoryOnServerLogEvent log_event{story_full_id};
  return binlog_add(G()->td_db()->get_binlog(), LogEvent::HandlerType::DeleteStoryOnServer,
                    get_log_event_storer(log_event));
}

void StoryManager::delete_story_on_server(StoryFullId story_full_id, uint64 log_event_id, Promise<Unit> &&promise) {
  LOG(INFO) << "Delete " << story_full_id << " from server";
  CHECK(story_full_id.is_valid());

  if (log_event_id == 0) {
    log_event_id = save_delete_story_on_server_log_event(story_full_id);
  }

  auto new_promise = get_erase_log_event_promise(log_event_id, std::move(promise));
  promise = std::move(new_promise);  // to prevent self-move

  deleted_story_full_ids_.insert(story_full_id);

  td_->create_handler<DeleteStoriesQuery>(std::move(promise))->send({story_full_id.get_story_id()});

  on_delete_story(story_full_id);
}

telegram_api::object_ptr<telegram_api::InputMedia> StoryManager::get_input_media(StoryFullId story_full_id) const {
  auto dialog_id = story_full_id.get_dialog_id();
  CHECK(dialog_id.get_type() == DialogType::User);
  auto r_input_user = td_->contacts_manager_->get_input_user(dialog_id.get_user_id());
  if (r_input_user.is_error()) {
    return nullptr;
  }
  return telegram_api::make_object<telegram_api::inputMediaStory>(r_input_user.move_as_ok(),
                                                                  story_full_id.get_story_id().get());
}

void StoryManager::remove_story_notifications_by_story_ids(DialogId dialog_id, const vector<StoryId> &story_ids) {
  VLOG(notifications) << "Trying to remove notification about " << story_ids << " in " << dialog_id;
  for (auto story_id : story_ids) {
    StoryFullId story_full_id{dialog_id, story_id};
    if (!have_story_force(story_full_id)) {
      LOG(INFO) << "Can't delete " << story_full_id << " because it is not found";
      // call synchronously to remove them before ProcessPush returns
      // td_->notification_manager_->remove_temporary_notification_by_story_id(
      //    story_notification_group_id, story_full_id, true, "remove_story_notifications_by_story_ids");
      continue;
    }
    on_delete_story(story_full_id);
  }
}

void StoryManager::get_current_state(vector<td_api::object_ptr<td_api::Update>> &updates) const {
  active_stories_.foreach([&](const DialogId &dialog_id, const unique_ptr<ActiveStories> &active_stories) {
    updates.push_back(get_update_chat_active_stories(dialog_id, active_stories.get()));
  });
  if (!td_->auth_manager_->is_bot()) {
    for (auto story_list_id : {StoryListId::main(), StoryListId::archive()}) {
      const auto &story_list = get_story_list(story_list_id);
      if (story_list.sent_total_count_ != -1) {
        updates.push_back(get_update_story_list_chat_count_object(story_list_id, story_list));
      }
    }
  }
}

void StoryManager::on_binlog_events(vector<BinlogEvent> &&events) {
  if (G()->close_flag()) {
    return;
  }
  bool have_old_message_database = G()->use_message_database() && !G()->td_db()->was_dialog_db_created();
  for (auto &event : events) {
    CHECK(event.id_ != 0);
    switch (event.type_) {
      case LogEvent::HandlerType::DeleteStoryOnServer: {
        DeleteStoryOnServerLogEvent log_event;
        log_event_parse(log_event, event.get_data()).ensure();

        auto dialog_id = log_event.story_full_id_.get_dialog_id();
        if (dialog_id != DialogId(td_->contacts_manager_->get_my_id())) {
          binlog_erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        td_->messages_manager_->have_dialog_force(dialog_id, "DeleteStoryOnServerLogEvent");
        delete_story_on_server(log_event.story_full_id_, event.id_, Auto());
        break;
      }
      case LogEvent::HandlerType::ReadStoriesOnServer: {
        ReadStoriesOnServerLogEvent log_event;
        log_event_parse(log_event, event.get_data()).ensure();

        auto dialog_id = log_event.dialog_id_;
        if (!td_->messages_manager_->have_dialog_force(dialog_id, "ReadStoriesOnServerLogEvent")) {
          binlog_erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }
        auto max_read_story_id = log_event.max_story_id_;
        auto active_stories = get_active_stories_force(dialog_id, "ReadStoriesOnServerLogEvent");
        if (active_stories == nullptr) {
          max_read_story_ids_[dialog_id] = max_read_story_id;
          if (dialog_id.get_type() == DialogType::User) {
            td_->contacts_manager_->on_update_user_max_read_story_id(dialog_id.get_user_id(), max_read_story_id);
          }
        } else {
          auto story_ids = active_stories->story_ids_;
          on_update_active_stories(dialog_id, max_read_story_id, std::move(story_ids), Promise<Unit>(),
                                   "ReadStoriesOnServerLogEvent");
        }
        read_stories_on_server(dialog_id, max_read_story_id, event.id_);
        break;
      }
      case LogEvent::HandlerType::LoadDialogExpiringStories: {
        LoadDialogExpiringStoriesLogEvent log_event;
        log_event_parse(log_event, event.get_data()).ensure();

        auto dialog_id = log_event.dialog_id_;
        if (!td_->messages_manager_->have_dialog_force(dialog_id, "LoadDialogExpiringStoriesLogEvent")) {
          binlog_erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }
        load_dialog_expiring_stories(dialog_id, event.id_, "LoadDialogExpiringStoriesLogEvent");
        break;
      }
      case LogEvent::HandlerType::SendStory: {
        if (!have_old_message_database) {
          binlog_erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        SendStoryLogEvent log_event;
        log_event_parse(log_event, event.get_data()).ensure();

        auto pending_story = std::move(log_event.pending_story_out_);
        pending_story->log_event_id_ = event.id_;

        CHECK(pending_story->story_->content_ != nullptr);
        if (pending_story->story_->content_->get_type() == StoryContentType::Unsupported) {
          LOG(ERROR) << "Sent story content is invalid: " << format::as_hex_dump<4>(event.get_data());
          binlog_erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        Dependencies dependencies;
        add_pending_story_dependencies(dependencies, pending_story.get());
        if (!dependencies.resolve_force(td_, "SendStoryLogEvent")) {
          binlog_erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        ++send_story_count_;
        CHECK(!pending_story->story_id_.is_server());
        pending_story->send_story_num_ = send_story_count_;
        pending_story->story_->content_ = dup_story_content(td_, pending_story->story_->content_.get());
        yet_unsent_stories_.insert(pending_story->send_story_num_);
        do_send_story(std::move(pending_story), {});
        break;
      }
      case LogEvent::HandlerType::EditStory: {
        if (!have_old_message_database) {
          binlog_erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        EditStoryLogEvent log_event;
        log_event_parse(log_event, event.get_data()).ensure();

        auto pending_story = std::move(log_event.pending_story_out_);
        CHECK(pending_story->story_id_.is_server());
        StoryFullId story_full_id{pending_story->dialog_id_, pending_story->story_id_};
        const Story *story = get_story_force(story_full_id, "EditStoryLogEvent");
        if (story == nullptr || story->content_ == nullptr) {
          LOG(INFO) << "Failed to find " << story_full_id;
          binlog_erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        if (pending_story->story_->content_ != nullptr &&
            pending_story->story_->content_->get_type() == StoryContentType::Unsupported) {
          LOG(ERROR) << "Sent story content is invalid: " << format::as_hex_dump<4>(event.get_data());
          binlog_erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        Dependencies dependencies;
        add_pending_story_dependencies(dependencies, pending_story.get());
        if (!dependencies.resolve_force(td_, "EditStoryLogEvent")) {
          binlog_erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }

        auto &edited_story = being_edited_stories_[story_full_id];
        if (edited_story != nullptr) {
          LOG(INFO) << "Ignore outdated edit of " << story_full_id;
          binlog_erase(G()->td_db()->get_binlog(), event.id_);
          break;
        }
        edited_story = make_unique<BeingEditedStory>();
        if (pending_story->story_->content_ != nullptr) {
          edited_story->content_ = std::move(pending_story->story_->content_);
        }
        if (log_event.edit_caption_) {
          edited_story->caption_ = std::move(log_event.caption_);
          edited_story->edit_caption_ = true;
        }
        edited_story->log_event_id_ = event.id_;

        ++send_story_count_;
        pending_story->send_story_num_ = std::numeric_limits<uint32>::max() - send_story_count_;
        pending_story->random_id_ = ++edit_generations_[story_full_id];

        if (edited_story->content_ == nullptr) {
          do_edit_story(FileId(), std::move(pending_story), nullptr);
        } else {
          pending_story->story_->content_ = dup_story_content(td_, edited_story->content_.get());
          do_send_story(std::move(pending_story), {});
        }
        break;
      }
      default:
        LOG(FATAL) << "Unsupported log event type " << event.type_;
    }
  }
}

}  // namespace td

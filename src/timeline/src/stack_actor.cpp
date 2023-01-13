// SPDX-License-Identifier: Apache-2.0
#include <caf/policy/select_all.hpp>

#include "xstudio/atoms.hpp"
#include "xstudio/broadcast/broadcast_actor.hpp"
#include "xstudio/timeline/clip_actor.hpp"
#include "xstudio/timeline/stack_actor.hpp"
#include "xstudio/timeline/gap_actor.hpp"
#include "xstudio/timeline/timeline_actor.hpp"
#include "xstudio/timeline/track_actor.hpp"
#include "xstudio/utility/helpers.hpp"
#include "xstudio/utility/logging.hpp"
#include "xstudio/utility/uuid.hpp"

using namespace xstudio;
using namespace xstudio::utility;
using namespace xstudio::timeline;
using namespace caf;

caf::actor StackActor::deserialise(const utility::JsonStore &value, const bool replace_item) {
    auto key   = utility::Uuid(value["base"]["item"]["uuid"]);
    auto actor = caf::actor();
    if (value["base"]["container"]["type"] == "Track") {
        auto item = Item();
        actor     = spawn<TrackActor>(static_cast<utility::JsonStore>(value), item);
        add_item(UuidActor(key, actor));
        if (replace_item) {
            auto itemit = find_uuid(base_.item().children(), key);
            (*itemit)   = item;
        }
    } else if (value["base"]["container"]["type"] == "Clip") {
        auto item = Item();
        actor     = spawn<ClipActor>(static_cast<utility::JsonStore>(value), item);
        add_item(UuidActor(key, actor));
        if (replace_item) {
            auto itemit = find_uuid(base_.item().children(), key);
            (*itemit)   = item;
        }
    } else if (value["base"]["container"]["type"] == "Gap") {
        auto item = Item();
        actor     = spawn<GapActor>(static_cast<utility::JsonStore>(value), item);
        add_item(UuidActor(key, actor));
        if (replace_item) {
            auto itemit = find_uuid(base_.item().children(), key);
            (*itemit)   = item;
        }
    } else if (value["base"]["container"]["type"] == "Stack") {
        auto item = Item();
        actor     = spawn<StackActor>(static_cast<utility::JsonStore>(value), item);
        add_item(UuidActor(key, actor));
        if (replace_item) {
            auto itemit = find_uuid(base_.item().children(), key);
            (*itemit)   = item;
        }
    }
    return actor;
}


StackActor::StackActor(caf::actor_config &cfg, const utility::JsonStore &jsn, Item &pitem)
    : caf::event_based_actor(cfg), base_(static_cast<utility::JsonStore>(jsn["base"])) {

    base_.item().set_actor_addr(this);

    for (const auto &[key, value] : jsn["actors"].items()) {
        try {
            deserialise(value, true);
        } catch (const std::exception &e) {
            spdlog::error("{}", e.what());
        }
    }
    base_.item().set_system(&system());
    base_.item().bind_item_event_func([this](const utility::JsonStore &event, Item &item) {
        item_event_callback(event, item);
    });
    pitem = base_.item();

    init();
}

StackActor::StackActor(
    caf::actor_config &cfg, const std::string &name, const utility::Uuid &uuid)
    : caf::event_based_actor(cfg), base_(name, uuid, this) {

    base_.item().set_system(&system());
    base_.item().bind_item_event_func([this](const utility::JsonStore &event, Item &item) {
        item_event_callback(event, item);
    });
    init();
}

void StackActor::on_exit() {
    for (const auto &i : actors_)
        send_exit(i.second, caf::exit_reason::user_shutdown);
}

// trigger actor creation
void StackActor::item_event_callback(const utility::JsonStore &event, Item &item) {

    switch (static_cast<ItemAction>(event["action"])) {
    case IT_INSERT: {
        auto cuuid = utility::Uuid(event["item"]["uuid"]);
        // spdlog::warn("{} {} {} {}", find_uuid(base_.item().children(), cuuid) !=
        // base_.item().cend(), actors_.count(cuuid), not event["blind"].is_null(),
        // event.dump(2)); needs to be child..
        auto child_item_it = find_uuid(base_.item().children(), cuuid);
        if (child_item_it != base_.item().cend() and not actors_.count(cuuid) and
            not event["blind"].is_null()) {
            // our child
            // spdlog::warn("RECREATE MATCH");

            auto actor = deserialise(utility::JsonStore(event["blind"]), false);
            add_item(UuidActor(cuuid, actor));
            // spdlog::warn("{}",to_string(caf::actor_cast<caf::actor_addr>(actor)));
            // spdlog::warn("{}",to_string(caf::actor_cast<caf::actor_addr>(child_item_it->actor())));
            child_item_it->set_actor_addr(actor);
            // change item actor addr
            // spdlog::warn("{}",to_string(caf::actor_cast<caf::actor_addr>(child_item_it->actor())));

            // item actor_addr will be wrong.. in ancestors
            // send special update..
            send(
                event_group_,
                event_atom_v,
                item_atom_v,
                child_item_it->make_actor_addr_update(),
                true);
        }
    } break;

    case IT_REMOVE: {
        auto cuuid = utility::Uuid(event["item_uuid"]);
        // child destroyed
        if (actors_.count(cuuid)) {
            // spdlog::warn("destroy
            // {}",to_string(caf::actor_cast<caf::actor_addr>(actors_[cuuid])));
            demonitor(actors_[cuuid]);
            send_exit(actors_[cuuid], caf::exit_reason::user_shutdown);
            actors_.erase(cuuid);
        }
    } break;

    case IT_ENABLE:
    case IT_ACTIVE:
    case IT_AVAIL:
    case IT_SPLICE:
    case IT_ADDR:
    case IA_NONE:
    default:
        break;
    }
}

void StackActor::init() {
    print_on_create(this, base_.name());
    print_on_exit(this, base_.name());

    event_group_ = spawn<broadcast::BroadcastActor>(this);
    link_to(event_group_);

    set_down_handler([=](down_msg &msg) {
        // if a child dies we won't have enough information to recreate it.
        // we still need to report it up the chain though.
        for (auto it = std::begin(actors_); it != std::end(actors_); ++it) {
            if (msg.source == it->second) {
                demonitor(it->second);
                actors_.erase(it);

                // remove from base.
                auto it = find_actor_addr(base_.item().children(), msg.source);

                if (it != base_.item().end()) {
                    auto jsn  = base_.item().erase(it);
                    auto more = base_.item().refresh();
                    if (not more.is_null())
                        jsn.insert(jsn.begin(), more.begin(), more.end());

                    send(event_group_, event_atom_v, item_atom_v, jsn, false);
                }
                break;
            }
        }
    });

    behavior_.assign(
        base_.make_set_name_handler(event_group_, this),
        base_.make_get_name_handler(),
        base_.make_last_changed_getter(),
        base_.make_last_changed_setter(event_group_, this),
        base_.make_last_changed_event_handler(event_group_, this),
        base_.make_get_uuid_handler(),
        base_.make_get_type_handler(),
        make_get_event_group_handler(event_group_),
        base_.make_get_detail_handler(this, event_group_),

        [=](xstudio::broadcast::broadcast_down_atom, const caf::actor_addr &) {
            // should we handle child down ?
        },

        [=](link_media_atom, const UuidActorMap &media) -> result<bool> {
            auto rp = make_response_promise<bool>();

            // pool direct children for state.
            fan_out_request<policy::select_all>(
                map_value_to_vec(actors_), infinite, link_media_atom_v, media)
                .await(
                    [=](std::vector<bool> items) mutable { rp.deliver(true); },
                    [=](error &err) mutable { rp.deliver(err); });

            return rp;
        },

        [=](item_atom) -> Item { return base_.item(); },

        [=](item_atom, const bool with_state) -> result<std::pair<JsonStore, Item>> {
            auto rp = make_response_promise<std::pair<JsonStore, Item>>();
            request(caf::actor_cast<caf::actor>(this), infinite, utility::serialise_atom_v)
                .then(
                    [=](const JsonStore &jsn) mutable {
                        rp.deliver(std::make_pair(jsn, base_.item()));
                    },
                    [=](const caf::error &err) mutable { rp.deliver(err); });
            return rp;
        },

        [=](item_atom, int index) -> result<Item> {
            if (static_cast<size_t>(index) >= base_.item().size()) {
                return make_error(xstudio_error::error, "Invalid index");
            }
            auto it = base_.item().cbegin();
            std::advance(it, index);
            return *it;
        },

        [=](plugin_manager::enable_atom, const bool value) -> JsonStore {
            auto jsn = base_.item().set_enabled(value);
            if (not jsn.is_null())
                send(event_group_, event_atom_v, item_atom_v, jsn, false);
            return jsn;
        },

        [=](active_range_atom, const FrameRange &fr) -> JsonStore {
            auto jsn = base_.item().set_active_range(fr);
            if (not jsn.is_null())
                send(event_group_, event_atom_v, item_atom_v, jsn, false);
            return jsn;
        },

        [=](available_range_atom, const FrameRange &fr) -> JsonStore {
            auto jsn = base_.item().set_available_range(fr);
            if (not jsn.is_null())
                send(event_group_, event_atom_v, item_atom_v, jsn, false);
            return jsn;
        },

        [=](history::undo_atom, const JsonStore &hist) -> result<bool> {
            base_.item().undo(hist);
            if (actors_.empty())
                return true;
            // push to children..
            auto rp = make_response_promise<bool>();

            fan_out_request<policy::select_all>(
                map_value_to_vec(actors_), infinite, history::undo_atom_v, hist)
                .then(
                    [=](std::vector<bool> updated) mutable { rp.deliver(true); },
                    [=](error &err) mutable { rp.deliver(std::move(err)); });

            return rp;
        },

        [=](history::redo_atom, const JsonStore &hist) -> result<bool> {
            base_.item().redo(hist);
            if (actors_.empty())
                return true;
            // push to children..
            auto rp = make_response_promise<bool>();

            fan_out_request<policy::select_all>(
                map_value_to_vec(actors_), infinite, history::redo_atom_v, hist)
                .then(
                    [=](std::vector<bool> updated) mutable { rp.deliver(true); },
                    [=](error &err) mutable { rp.deliver(std::move(err)); });

            return rp;
        },

        // handle child change events.
        // [=](event_atom, item_atom, const Item &item) {
        //     // it's possibly one of ours.. so try and substitue the record
        //     if(base_.item().replace_child(item)) {
        //         base_.item().refresh();
        //         send(event_group_, event_atom_v, item_atom_v, base_.item());
        //     }
        // },

        // handle child change events.
        [=](event_atom, item_atom, const JsonStore &update, const bool hidden) {
            if (base_.item().update(update)) {
                auto more = base_.item().refresh();
                if (not more.is_null()) {
                    more.insert(more.begin(), update.begin(), update.end());
                    send(event_group_, event_atom_v, item_atom_v, more, hidden);
                    return;
                }
            }

            send(event_group_, event_atom_v, item_atom_v, update, hidden);
        },


        [=](insert_item_atom, const int index, const UuidActor &ua) -> result<JsonStore> {
            auto rp = make_response_promise<JsonStore>();
            // get item..
            request(ua.actor(), infinite, item_atom_v)
                .then(
                    [=](const Item &item) mutable {
                        rp.delegate(
                            caf::actor_cast<caf::actor>(this),
                            insert_item_atom_v,
                            index,
                            ua,
                            item);
                    },
                    [=](const caf::error &err) mutable { rp.deliver(err); });

            return rp;
        },

        // we only allow access to direct children.. ?
        [=](insert_item_atom, const int index, const UuidActor &ua, const Item &item)
            -> result<JsonStore> {
            if (not base_.item().valid_child(item)) {
                return make_error(xstudio_error::error, "Invalid child type");
            }

            // take ownership
            add_item(ua);

            auto rp = make_response_promise<JsonStore>();
            // re-aquire item. as we may have gone out of sync..
            request(ua.actor(), infinite, item_atom_v)
                .await(
                    [=](const Item &item) mutable {
                        // insert on index..
                        // cheat..
                        auto it  = base_.item().begin();
                        auto ind = 0;
                        for (int i = 0; it != base_.item().end(); i++, it++) {
                            if (i == index)
                                break;
                        }

                        auto changes = base_.item().insert(it, item);
                        auto more    = base_.item().refresh();
                        if (not more.is_null())
                            changes.insert(changes.begin(), more.begin(), more.end());

                        send(event_group_, event_atom_v, item_atom_v, changes, false);
                        rp.deliver(changes);
                    },
                    [=](const caf::error &err) mutable { rp.deliver(err); });

            return rp;
        },


        [=](insert_item_atom,
            const utility::Uuid &before_uuid,
            const UuidActor &ua) -> result<JsonStore> {
            auto rp = make_response_promise<JsonStore>();
            // get item..
            request(ua.actor(), infinite, item_atom_v)
                .then(
                    [=](const Item &item) mutable {
                        rp.delegate(
                            caf::actor_cast<caf::actor>(this),
                            insert_item_atom_v,
                            before_uuid,
                            ua,
                            item);
                    },
                    [=](const caf::error &err) mutable { rp.deliver(err); });

            return rp;
        },

        [=](insert_item_atom,
            const utility::Uuid &before_uuid,
            const UuidActor &ua,
            const Item &item) -> result<JsonStore> {
            if (not base_.item().valid_child(item)) {
                return make_error(xstudio_error::error, "Invalid child type");
            }
            // take ownership
            add_item(ua);

            auto rp = make_response_promise<JsonStore>();
            // re-aquire item. as we may have gone out of sync..
            request(ua.actor(), infinite, item_atom_v)
                .await(
                    [=](const Item &item) mutable {
                        auto changes = utility::JsonStore();

                        if (before_uuid.is_null()) {
                            changes = base_.item().insert(base_.item().end(), item);
                        } else {
                            auto it = find_uuid(base_.item().children(), before_uuid);
                            if (it == base_.item().end()) {
                                return rp.deliver(
                                    make_error(xstudio_error::error, "Invalid uuid"));
                            }
                            changes = base_.item().insert(it, item);
                        }

                        auto more = base_.item().refresh();
                        if (not more.is_null())
                            changes.insert(changes.begin(), more.begin(), more.end());

                        send(event_group_, event_atom_v, item_atom_v, changes, false);
                        rp.deliver(changes);
                    },
                    [=](const caf::error &err) mutable { rp.deliver(err); });

            return rp;
        },

        [=](move_item_atom, const int src_index, const int count, const int dst_index)
            -> result<JsonStore> {
            auto sit = base_.item().children().begin();
            std::advance(sit, src_index);
            if (sit == base_.item().children().end())
                return make_error(xstudio_error::error, "Invalid src index");
            auto src_uuid = sit->uuid();

            auto dit = base_.item().children().begin();
            std::advance(dit, dst_index);
            auto dst_uuid = utility::Uuid();
            if (dit != base_.item().children().end())
                dst_uuid = dit->uuid();

            auto rp = make_response_promise<JsonStore>();
            rp.delegate(
                caf::actor_cast<caf::actor>(this), move_item_atom_v, src_uuid, count, dst_uuid);
            return rp;
        },

        [=](move_item_atom,
            const utility::Uuid &src_uuid,
            const int count,
            const utility::Uuid &before_uuid) -> result<JsonStore> {
            // check src is valid.
            auto sitb = find_uuid(base_.item().children(), src_uuid);
            if (sitb == base_.item().end())
                return make_error(xstudio_error::error, "Invalid src uuid");

            auto dit = base_.item().children().end();
            if (not before_uuid.is_null()) {
                dit = find_uuid(base_.item().children(), before_uuid);
                std::advance(dit, 1);
                if (dit == base_.item().end())
                    return make_error(xstudio_error::error, "Invalid dst uuid");
            }

            if (count) {
                auto site = sitb;
                std::advance(site, count);

                // auto isitb = std::distance(base_.item().children().begin(), sitb);
                // auto isite = std::distance(base_.item().children().begin(), site);
                // auto idit = std::distance(base_.item().children().begin(), dit);

                // spdlog::warn("src_start {}", isitb);
                // spdlog::warn("src_end {}", isite);
                // spdlog::warn("dst {}", idit);
                auto changes = base_.item().splice(dit, base_.item().children(), sitb, site);
                auto more    = base_.item().refresh();
                if (not more.is_null())
                    changes.insert(changes.begin(), more.begin(), more.end());

                send(event_group_, event_atom_v, item_atom_v, changes, false);
                return changes;
            }

            return JsonStore();
        },

        [=](remove_item_atom, const int index) -> result<std::pair<JsonStore, Item>> {
            auto it = base_.item().children().begin();
            std::advance(it, index);
            if (it == base_.item().children().end())
                return make_error(xstudio_error::error, "Invalid index");

            auto rp = make_response_promise<std::pair<JsonStore, Item>>();
            rp.delegate(caf::actor_cast<caf::actor>(this), remove_item_atom_v, it->uuid());
            return rp;
        },

        [=](remove_item_atom, const utility::Uuid &uuid) -> result<std::pair<JsonStore, Item>> {
            auto it = find_uuid(base_.item().children(), uuid);
            if (it == base_.item().end())
                return make_error(xstudio_error::error, "Invalid uuid");

            auto item = *it;
            demonitor(item.actor());
            actors_.erase(item.uuid());

            auto changes = base_.item().erase(it);
            auto more    = base_.item().refresh();
            if (not more.is_null())
                changes.insert(changes.begin(), more.begin(), more.end());

            send(event_group_, event_atom_v, item_atom_v, changes, false);

            // as the item/actor still exists.. ?
            // ACK!!!! What do we do !!!
            return std::make_pair(changes, item);
        },

        [=](erase_item_atom, const int index) -> result<JsonStore> {
            auto it = base_.item().children().begin();
            std::advance(it, index);
            if (it == base_.item().children().end())
                return make_error(xstudio_error::error, "Invalid index");
            auto rp = make_response_promise<JsonStore>();
            rp.delegate(caf::actor_cast<caf::actor>(this), erase_item_atom_v, it->uuid());
            return rp;
        },

        [=](erase_item_atom, const utility::Uuid &uuid) -> result<JsonStore> {
            auto rp = make_response_promise<JsonStore>();
            request(caf::actor_cast<caf::actor>(this), infinite, remove_item_atom_v, uuid)
                .then(
                    [=](const std::pair<JsonStore, Item> &hist_item) mutable {
                        send_exit(hist_item.second.actor(), caf::exit_reason::user_shutdown);
                        rp.deliver(hist_item.first);
                    },
                    [=](error &err) mutable { rp.deliver(std::move(err)); });
            return rp;
        },

        [=](utility::serialise_atom) -> result<JsonStore> {
            if (not actors_.empty()) {
                auto rp = make_response_promise<JsonStore>();

                fan_out_request<policy::select_all>(
                    map_value_to_vec(actors_), infinite, serialise_atom_v)
                    .then(
                        [=](std::vector<JsonStore> json) mutable {
                            JsonStore jsn;
                            jsn["base"]   = base_.serialise();
                            jsn["actors"] = {};
                            for (const auto &j : json)
                                jsn["actors"]
                                   [static_cast<std::string>(j["base"]["container"]["uuid"])] =
                                       j;
                            rp.deliver(jsn);
                        },
                        [=](error &err) mutable { rp.deliver(std::move(err)); });

                return rp;
            }
            JsonStore jsn;
            jsn["base"]   = base_.serialise();
            jsn["actors"] = {};

            return result<JsonStore>(jsn);
        });
}

void StackActor::add_item(const utility::UuidActor &ua) {
    // joining must be in sync
    scoped_actor sys{system()};

    try {
        auto grp =
            request_receive<caf::actor>(*sys, ua.actor(), utility::get_event_group_atom_v);
        auto joined = request_receive<bool>(*sys, grp, broadcast::join_broadcast_atom_v, this);
    } catch (const std::exception &err) {
        spdlog::warn("{} {}", __PRETTY_FUNCTION__, err.what());
    }

    // join_event_group(this, ua.second);
    monitor(ua.actor());
    actors_[ua.uuid()] = ua.actor();
}

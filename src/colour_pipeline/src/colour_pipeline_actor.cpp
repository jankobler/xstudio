// SPDX-License-Identifier: Apache-2.0
#include <caf/sec.hpp>

#include "xstudio/atoms.hpp"
#include "xstudio/colour_pipeline/colour_pipeline_actor.hpp"
#include "xstudio/global_store/global_store.hpp"
#include "xstudio/utility/edit_list.hpp"
#include "xstudio/utility/helpers.hpp"
#include "xstudio/utility/json_store.hpp"
#include "xstudio/utility/logging.hpp"
#include "xstudio/utility/types.hpp"

using namespace std::chrono_literals;
using namespace caf;

using namespace xstudio;
using namespace xstudio::colour_pipeline;
using namespace xstudio::global_store;
using namespace xstudio::utility;

GlobalColourPipelineActor::GlobalColourPipelineActor(caf::actor_config &cfg)
    : caf::event_based_actor(cfg), Module("GlobalColourPipelineActor") {
    system().registry().put(colour_pipeline_registry, this);

    // Read configuration file
    try {
        auto prefs = GlobalStoreHelper(system());
        join_broadcast(this, prefs.get_group(prefs_jsn_));

        default_plugin_name_ =
            preference_value<std::string>(prefs_jsn_, "/core/colour_pipeline/default_pipeline");
    } catch (...) {
        default_plugin_name_ = BUILTIN_PLUGIN_NAME;
    }

    load_colour_pipe_details();

    set_parent_actor_addr(actor_cast<caf::actor_addr>(this));
}

caf::behavior GlobalColourPipelineActor::make_behavior() {
    return caf::message_handler{
        [=](xstudio::broadcast::broadcast_down_atom, const caf::actor_addr &) {
            // nop
        },
        [=](get_colour_pipeline_atom, const std::string &pipe_name) -> result<caf::actor> {
            auto rp = make_response_promise<caf::actor>();
            make_or_get_colour_pipeline(pipe_name, prefs_jsn_, rp);
            return rp;
        },
        [=](get_colour_pipeline_atom) -> result<caf::actor> {
            auto rp = make_response_promise<caf::actor>();
            make_or_get_colour_pipeline(default_plugin_name_, prefs_jsn_, rp);
            return rp;
        },
        [=](json_store::update_atom,
            const JsonStore & /*change*/,
            const std::string & /*path*/,
            const JsonStore &full) {
            delegate(actor_cast<caf::actor>(this), json_store::update_atom_v, full);
        },
        [=](json_store::update_atom, const JsonStore &js) { prefs_jsn_ = js; },
        [=](module::connect_to_ui_atom, caf::actor cpipe) {
            // as it stands we only ever want one colour pipeline plugin exposing
            // its attributes (via dynamic widgets) in the UI at once. As such,
            // this global colour pipe actor will manage which colour pipe is
            // 'connected' to the UI, because playheads may or may not share a
            // colour pipeline actor and it would be complicated for playheads
            // to switch on/off the active status of the colour pipeline actors
            // without sync issues getting us into a mess
            if (cpipe != active_in_ui_colour_pipeline_) {
                if (active_in_ui_colour_pipeline_) {
                    anon_send(active_in_ui_colour_pipeline_, module::disconnect_from_ui_atom_v);
                }
                active_in_ui_colour_pipeline_ = cpipe;
                if (active_in_ui_colour_pipeline_) {
                    anon_send(active_in_ui_colour_pipeline_, module::connect_to_ui_atom_v);
                }
            }
        },
        [=](media_reader::process_thumbnail_atom,
            const media::AVFrameID &mptr,
            const thumbnail::ThumbnailBufferPtr &buf) -> result<thumbnail::ThumbnailBufferPtr> {
            auto rp = make_response_promise<thumbnail::ThumbnailBufferPtr>();

            request(caf::actor_cast<caf::actor>(this), infinite, get_colour_pipeline_atom_v)
                .then(
                    [=](caf::actor colour_pipe) mutable {
                        rp.delegate(
                            colour_pipe, media_reader::process_thumbnail_atom_v, mptr, buf);
                    },
                    [=](caf::error &err) mutable { rp.deliver(err); });
            return rp;
        }};
}

void GlobalColourPipelineActor::load_colour_pipe_details() {
    scoped_actor sys{system()};
    bool matched = false;

    try {
        auto pm = system().registry().template get<caf::actor>(plugin_manager_registry);
        colour_pipe_plugin_details_ =
            request_receive<std::vector<plugin_manager::PluginDetail>>(
                *sys,
                pm,
                utility::detail_atom_v,
                plugin_manager::PluginType::PT_COLOUR_MANAGEMENT);

        for (const auto &pd : colour_pipe_plugin_details_) {
            if (pd.enabled_ && pd.name_ == default_plugin_name_) {
                matched = true;
                break;
            }
        }
    } catch (std::exception &e) {
        spdlog::warn("{} {}", __PRETTY_FUNCTION__, e.what());
    }
    if (not matched) {
        spdlog::warn(
            "{} ColourPipeline not available {}", __PRETTY_FUNCTION__, default_plugin_name_);
        default_plugin_name_ = BUILTIN_PLUGIN_NAME;
    }
}

void GlobalColourPipelineActor::make_or_get_colour_pipeline(
    const std::string &pipe_name,
    const utility::JsonStore &jsn,
    caf::typed_response_promise<caf::actor> &rp) {
    // Look in loaded actors first
    if (colour_pipeline_actors_.find(pipe_name) != colour_pipeline_actors_.end()) {
        rp.deliver(colour_pipeline_actors_[pipe_name]);
        return;
    }

    // Otherwise try load the requested plugin and fallback to the builtin
    // OCIO plugin if loading fails
    utility::Uuid uuid;
    for (const auto &i : colour_pipe_plugin_details_) {
        if (i.name_ == pipe_name) {
            uuid = i.uuid_;
            break;
        }
    }

    if (uuid.is_null()) {
        rp.deliver(make_error(
            xstudio_error::error,
            "create_colour_pipeline failed, invalid colour pipeline name."));
    } else {
        auto pm = system().registry().template get<caf::actor>(plugin_manager_registry);
        request(pm, infinite, plugin_manager::spawn_plugin_atom_v, uuid, jsn)
            .await(
                [=](caf::actor colour_pipe) mutable {
                    link_to(colour_pipe);
                    colour_pipeline_actors_[pipe_name] = colour_pipe;
                    rp.deliver(colour_pipe);
                },
                [=](const error &err) mutable {
                    if (pipe_name == default_plugin_name_ and
                        default_plugin_name_ != BUILTIN_PLUGIN_NAME) {
                        spdlog::warn(
                            "{} ColourPipeline failed {} {}, fallback to {}",
                            __PRETTY_FUNCTION__,
                            default_plugin_name_,
                            to_string(err),
                            BUILTIN_PLUGIN_NAME);

                        default_plugin_name_ = BUILTIN_PLUGIN_NAME;
                        make_or_get_colour_pipeline(default_plugin_name_, jsn, rp);
                    } else {
                        rp.deliver(err);
                    }
                });
    }
}

void GlobalColourPipelineActor::on_exit() {
    system().registry().erase(colour_pipeline_registry);
}

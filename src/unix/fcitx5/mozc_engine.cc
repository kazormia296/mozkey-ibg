/*
 * Copyright (C) 2017~2017 by CSSlayer
 * wengxt@gmail.com
 * Modified by the Mozkey contributors on 2026-07-17 for the Grimodex Linux
 * integration, session isolation, and Zenz runtime capability reporting.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; see the file COPYING. If not,
 * see <http://www.gnu.org/licenses/>.
 */

#include "unix/fcitx5/mozc_engine.h"

#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/macros.h>
#include <fcitx-utils/misc.h>
#include <fcitx-utils/semver.h>
#include <fcitx-utils/stringutils.h>
#include <fcitx/action.h>
#include <fcitx/addoninstance.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/instance.h>
#include <fcitx/statusarea.h>
#include <fcitx/userinterface.h>
#include <fcitx/userinterfacemanager.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "absl/status/status.h"
#include "absl/time/time.h"
#include "base/environ.h"
#include "base/init_mozc.h"
#include "base/process.h"
#include "base/version.h"
#include "grimodex/protocol_v1_secure_reader.h"
#include "protocol/commands.pb.h"
#include "unix/fcitx5/grimodex_consumer_registrar.h"
#include "unix/fcitx5/i18nwrapper.h"
#include "unix/fcitx5/mozc_client_interface.h"
#include "unix/fcitx5/mozc_client_pool.h"
#include "unix/fcitx5/mozc_response_parser.h"
#include "unix/fcitx5/mozc_state.h"

namespace fcitx {

namespace {

constexpr uint64_t kGrimodexConsumerHeartbeatUsec = 15ULL * 60 * 1000 * 1000;
constexpr uint64_t kGrimodexConsumerTimerAccuracyUsec = 1000 * 1000;

std::string GrimodexConsumerTimestamp() {
  return absl::FormatTime("%Y-%m-%dT%H:%M:%E3SZ", absl::Now(),
                          absl::UTCTimeZone());
}

}  // namespace

const struct CompositionModeInfo {
  const char* name;
  const char* icon;
  const char* label;
  const char* description;
  mozc::commands::CompositionMode mode;
} kPropCompositionModes[] = {
    {
        "mozkey-ibg-mode-direct",
        "fcitx_mozkey_ibg_direct",
        "A",
        N_("Direct"),
        mozc::commands::DIRECT,
    },
    {
        "mozkey-ibg-mode-hiragana",
        "fcitx_mozkey_ibg_hiragana",
        "\xe3\x81\x82",  // Hiragana letter A in UTF-8.
        N_("Hiragana"),
        mozc::commands::HIRAGANA,
    },
    {
        "mozkey-ibg-mode-katakana_full",
        "fcitx_mozkey_ibg_katakana_full",
        "\xe3\x82\xa2",  // Katakana letter A.
        N_("Full Katakana"),
        mozc::commands::FULL_KATAKANA,
    },
    {

        "mozkey-ibg-mode-alpha_half",
        "fcitx_mozkey_ibg_alpha_half",
        "A",
        N_("Half ASCII"),
        mozc::commands::HALF_ASCII,
    },
    {

        "mozkey-ibg-mode-alpha_full",
        "fcitx_mozkey_ibg_alpha_full",
        "\xef\xbc\xa1",  // Full width ASCII letter A.
        N_("Full ASCII"),
        mozc::commands::FULL_ASCII,
    },
    {
        "mozkey-ibg-mode-katakana_half",
        "fcitx_mozkey_ibg_katakana_half",
        "\xef\xbd\xb1",  // Half width Katakana letter A.
        N_("Half Katakana"),
        mozc::commands::HALF_KATAKANA,
    },
};
const size_t kNumCompositionModes = FCITX_ARRAY_SIZE(kPropCompositionModes);

MozcModeSubAction::MozcModeSubAction(MozcEngine* engine,
                                     mozc::commands::CompositionMode mode)
    : engine_(engine), mode_(mode) {
  setShortText(_(kPropCompositionModes[mode].description));
  setLongText(_(kPropCompositionModes[mode].description));
  setIcon(kPropCompositionModes[mode].icon);
  setCheckable(true);
}

bool MozcModeSubAction::isChecked(InputContext* ic) const {
  auto* mozc_state = engine_->mozcState(ic);
  return mozc_state->GetCompositionMode() == mode_;
}

void MozcModeSubAction::activate(InputContext* ic) {
  auto* mozc_state = engine_->mozcState(ic);
  mozc_state->SendCompositionMode(mode_);
}

// This array must correspond with the CompositionMode enum in the
// mozc/session/command.proto file.
static_assert(mozc::commands::NUM_OF_COMPOSITIONS == kNumCompositionModes,
              "number of modes must match");

Instance* Init(Instance* instance) {
  int argc = 1;
  char argv0[] = "fcitx_mozkey_ibg";
  char* _argv[] = {argv0};
  char** argv = _argv;
  mozc::InitMozc(argv[0], &argc, &argv);
  return instance;
}

MozcEngine::MozcEngine(Instance* instance)
    : instance_(Init(instance)),
      parser_(std::make_unique<MozcResponseParser>(this)),
      factory_([this](InputContext& ic) { return new MozcState(&ic, this); }) {
  pool_ = std::make_unique<MozcClientPool>([] { return createClient(); });
  for (auto command :
       {mozc::commands::DIRECT, mozc::commands::HIRAGANA,
        mozc::commands::FULL_KATAKANA, mozc::commands::FULL_ASCII,
        mozc::commands::HALF_ASCII, mozc::commands::HALF_KATAKANA}) {
    modeActions_.push_back(std::make_unique<MozcModeSubAction>(this, command));
  }

  instance_->inputContextManager().registerProperty("mozkeyIbGState",
                                                    &factory_);
  instance_->userInterfaceManager().registerAction("mozkey-ibg-tool",
                                                   &toolAction_);
  toolAction_.setShortText(_("Mozkey IbG Settings"));
  toolAction_.setLongText(_("Mozkey IbG Settings"));
  toolAction_.setIcon("fcitx_mozkey_ibg_tool");

  int i = 0;
  for (auto& modeAction : modeActions_) {
    instance_->userInterfaceManager().registerAction(
        kPropCompositionModes[i].name, modeAction.get());
    toolMenu_.addAction(modeAction.get());
    i++;
  }

  separatorAction_.setSeparator(true);
  instance_->userInterfaceManager().registerAction("mozkey-ibg-separator",
                                                   &separatorAction_);

  SemanticVersion version;
  version.setMajor(5);
  version.setMinor(0);
  version.setPatch(22);
  // Where we fix the support for separator
  if (auto fcitxVersion = SemanticVersion::parse(Instance::version());
      fcitxVersion && *fcitxVersion >= version) {
    toolMenu_.addAction(&separatorAction_);
  }

  instance_->userInterfaceManager().registerAction("mozkey-ibg-tool-config",
                                                   &configToolAction_);
  configToolAction_.setShortText(_("Configuration Tool"));
  configToolAction_.setIcon("fcitx_mozkey_ibg_tool");
  configToolAction_.connect<SimpleAction::Activated>([](InputContext*) {
    mozc::Process::SpawnMozcProcess("mozc_tool", "--mode=config_dialog");
  });

  instance_->userInterfaceManager().registerAction("mozkey-ibg-tool-dict",
                                                   &dictionaryToolAction_);
  dictionaryToolAction_.setShortText(_("Dictionary Tool"));
  dictionaryToolAction_.setIcon("fcitx_mozkey_ibg_dictionary");
  dictionaryToolAction_.connect<SimpleAction::Activated>([](InputContext*) {
    mozc::Process::SpawnMozcProcess("mozc_tool", "--mode=dictionary_tool");
  });

  instance_->userInterfaceManager().registerAction("mozkey-ibg-tool-add",
                                                   &addWordAction_);
  addWordAction_.setShortText(_("Add Word"));
  addWordAction_.connect<SimpleAction::Activated>([](InputContext*) {
    mozc::Process::SpawnMozcProcess("mozc_tool", "--mode=word_register_dialog");
  });

  instance_->userInterfaceManager().registerAction("mozkey-ibg-tool-about",
                                                   &aboutAction_);
  aboutAction_.setShortText(_("About Mozkey IbG"));
  aboutAction_.connect<SimpleAction::Activated>([](InputContext*) {
    mozc::Process::SpawnMozcProcess("mozc_tool", "--mode=about_dialog");
  });

  toolMenu_.addAction(&configToolAction_);
  toolMenu_.addAction(&dictionaryToolAction_);
  toolMenu_.addAction(&addWordAction_);
  toolMenu_.addAction(&aboutAction_);

  toolAction_.setMenu(&toolMenu_);

  capabilityAboutToChangeHandle_ = instance_->watchEvent(
      EventType::InputContextCapabilityAboutToChange,
      EventWatcherPhase::PreInputMethod, [this](Event& event) {
        auto& capability_event =
            static_cast<CapabilityAboutToChangeEvent&>(event);
        InputContext* ic = capability_event.inputContext();
        if (instance_->inputMethod(ic) == "mozkey-ibg") {
          mozcState(ic)->CapabilityAboutToChange();
        }
      });
  capabilityChangedHandle_ = instance_->watchEvent(
      EventType::InputContextCapabilityChanged,
      EventWatcherPhase::PreInputMethod, [this](Event& event) {
        auto& capability_event = static_cast<CapabilityChangedEvent&>(event);
        InputContext* ic = capability_event.inputContext();
        if (instance_->inputMethod(ic) == "mozkey-ibg") {
          mozcState(ic)->CapabilityChanged();
        }
      });

  grimodex_consumer_registrar_ =
      std::make_unique<mozc::fcitx5::GrimodexConsumerRegistrar>(
          mozc::grimodex::ResolveProtocolV1Root(
              mozc::Environ::GetEnv("GRIMODEX_IME_ROOT"),
              mozc::Environ::GetEnv("XDG_DATA_HOME"),
              mozc::Environ::GetEnv("HOME")));
  const std::string runtime_marker = mozc::FileUtil::JoinPath(
      mozc::SystemUtil::GetServerDirectory(), "mozc_server");
  const auto refresh_consumer = [this, runtime_marker] {
    const absl::Status status =
        grimodex_consumer_registrar_->RefreshIfInstalled(
            mozc::Version::GetMozkeyReleaseVersion(),
            GrimodexConsumerTimestamp(), runtime_marker);
    if (!status.ok()) {
      FCITX_WARN() << "Failed to refresh Grimodex Mozkey consumer: "
                   << status;
    }
  };
  refresh_consumer();
  grimodex_consumer_heartbeat_ = instance_->eventLoop().addTimeEvent(
      CLOCK_BOOTTIME,
      fcitx::now(CLOCK_BOOTTIME) + kGrimodexConsumerHeartbeatUsec,
      kGrimodexConsumerTimerAccuracyUsec,
      [refresh_consumer](fcitx::EventSourceTime *timer, uint64_t) {
        refresh_consumer();
        timer->setNextInterval(kGrimodexConsumerHeartbeatUsec);
        timer->setOneShot();
        return true;
      });

  reloadConfig();
}

MozcEngine::~MozcEngine() {}

void MozcEngine::setConfig(const RawConfig& config) {
  config_.load(config, true);
  safeSaveAsIni(config_, "conf/mozkey-ibg.conf");
}

void MozcEngine::reloadConfig() {
  readAsIni(config_, "conf/mozkey-ibg.conf");
}
void MozcEngine::activate(const fcitx::InputMethodEntry& /*entry*/,
                          fcitx::InputContextEvent& event) {
  auto* ic = event.inputContext();
  auto* mozc_state = mozcState(ic);
  mozc_state->FocusIn();
  ic->statusArea().addAction(StatusGroup::InputMethod, &toolAction_);
}
void MozcEngine::deactivate(const fcitx::InputMethodEntry& /*entry*/,
                            fcitx::InputContextEvent& event) {
  auto* ic = event.inputContext();
  deactivating_ = true;
  auto* mozc_state = mozcState(ic);
  mozc_state->FocusOut(event);
  deactivating_ = false;
}
void MozcEngine::keyEvent(const InputMethodEntry& entry, KeyEvent& event) {
  auto* mozc_state = mozcState(event.inputContext());

  const auto& group = instance_->inputMethodManager().currentGroup();
  std::string layout = group.layoutFor(entry.uniqueName());
  if (layout.empty()) {
    layout = group.defaultLayout();
  }

  const bool isJP = (layout == "jp" || layout.starts_with("jp-"));

  if (mozc_state->ProcessKeyEvent(event.rawKey().sym(), event.rawKey().code(),
                                  event.rawKey().states(), isJP,
                                  event.isRelease())) {
    event.filterAndAccept();
  }
}

void MozcEngine::reset(const InputMethodEntry& /*entry*/,
                       InputContextEvent& event) {
  auto* mozc_state = mozcState(event.inputContext());
  mozc_state->Reset();
}

void MozcEngine::save() {
  // SyncData is not tied to an input domain.  Use a short-lived maintenance
  // client so no engine-global session can ever be shared by InputContexts.
  if (std::unique_ptr<MozcClientInterface> client = createClient()) {
    client->SyncData();
  }
}

std::string MozcEngine::subMode(const fcitx::InputMethodEntry& /*entry*/,
                                fcitx::InputContext& ic) {
  auto* mozc_state = mozcState(&ic);
  return _(kPropCompositionModes[mozc_state->GetCompositionMode()].description);
}

std::string MozcEngine::subModeIconImpl(
    const fcitx::InputMethodEntry& /*unused*/, fcitx::InputContext& ic) {
  auto* mozc_state = mozcState(&ic);
  return _(kPropCompositionModes[mozc_state->GetCompositionMode()].icon);
}

MozcState* MozcEngine::mozcState(InputContext* ic) {
  return ic->propertyFor(&factory_);
}

void MozcEngine::compositionModeUpdated(InputContext* ic) {
  for (const auto& modeAction : modeActions_) {
    modeAction->update(ic);
  }
  ic->updateUserInterface(UserInterfaceComponent::StatusArea);
}

AddonInstance* MozcEngine::clipboardAddon() { return clipboard(); }

}  // namespace fcitx

// Copyright 2010-2021, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#import "mac/mozc_imk_input_controller.h"

#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>
#import <InputMethodKit/IMKInputController.h>
#import <InputMethodKit/IMKServer.h>

#include <unistd.h>
#include <stdlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <utility>

#import "mac/KeyCodeMap.h"
#import "mac/renderer_receiver.h"
#include "mac/zenz_context_acquisition.h"

#include "absl/log/log.h"
#include "absl/strings/string_view.h"
#include "base/const.h"
#include "base/mac/mac_process.h"
#include "base/mac/mac_util.h"
#include "base/process.h"
#include "base/util.h"
#include "client/client.h"
#include "grimodex/client_context.h"
#include "ipc/ipc.h"
#include "protocol/candidate_window.pb.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "renderer/renderer_client.h"

using mozc::kProductNameInEnglish;
using mozc::MacProcess;
using mozc::commands::Capability;
using mozc::commands::CompositionMode;
using mozc::commands::KeyEvent;
using mozc::commands::Output;
using mozc::commands::Preedit;
using mozc::commands::RendererCommand;
using mozc::commands::SessionCommand;
using mozc::config::Config;
// less<> is necessary to compare between std::string and absl::string_view.
using SetOfString = std::set<std::string, std::less<>>;

@interface MozcImkInputController ()
- (void)cancelDelayedSessionCommand;
- (void)delayedInvokeSessionCommandFromTimer:(NSTimer *)timer;
- (void)dispatchSessionCommand:(const SessionCommand *)command client:(id)sender;
- (BOOL)isSecureEventInputEnabledForContext;
- (void)applyClientHintsToContext:(mozc::commands::Context *)context;
- (BOOL)refreshSecureInputState;
- (BOOL)isCurrentGrimodexEpoch:(uint64_t)focusEpoch;
- (BOOL)isCurrentNonSecureTextQueryEpoch:(uint64_t)focusEpoch;
- (void)advanceGrimodexFocusEpoch;
- (mozc::commands::Context)buildClientContextForClient:(id<IMKTextInput>)client
                               includeSurroundingText:(BOOL)includeSurroundingText;
- (BOOL)collectSurroundingContext:(mozc::commands::Context *)context
                           client:(id<IMKTextInput>)client;
- (void)resetContextForClient:(id<IMKTextInput>)client;
- (void)processOutput:(const mozc::commands::Output *)output
                 client:(id)sender
      sendingFocusEpoch:(uint64_t)sendingFocusEpoch;
- (BOOL)updateComposedString:(const mozc::commands::Preedit *)preedit
              liveConversion:(bool)liveConversion
           sendingFocusEpoch:(uint64_t)sendingFocusEpoch;
- (void)resetWritingDirection;
- (void)updateWritingDirectionFromAttributes:(NSDictionary *)attributes;
@end

namespace {
// Global object used as a singleton used as a proxy to receive messages from
// the renderer process.
RendererReceiver *gRendererReceiver = nil;

// TODO(horo): This value should be get from system configuration.
//  DoubleClickInterval can be get from NSEvent (MacOSX ver >= 10.6)
constexpr NSTimeInterval kDoubleTapInterval = 0.5;

uint64_t NewRendererCallbackToken() {
  uint64_t token = 0;
  do {
    ::arc4random_buf(&token, sizeof(token));
  } while (token == 0);
  return token;
}

constexpr int kMaxSurroundingLength = 20;
// Legacy generic surrounding-context acquisition keeps the historical client
// length cap because some applications make attributedSubstringFromRange:
// expensive on large documents. Zenz-specific acquisition is independently
// bounded to a small native range and does not inherit this document-size cap.
constexpr int kGenericSurroundingTextClientLengthLimit = 1000;

constexpr absl::string_view kRomanModeId = "com.apple.inputmethod.Roman";
constexpr absl::string_view kKatakanaModeId = "com.apple.inputmethod.Japanese.Katakana";
constexpr absl::string_view kHalfWidthKanaModeId = "com.apple.inputmethod.Japanese.HalfWidthKana";
constexpr absl::string_view kFullWidthRomanModeId = "com.apple.inputmethod.Japanese.FullWidthRoman";
constexpr absl::string_view kHiraganaModeId = "com.apple.inputmethod.Japanese";

CompositionMode GetCompositionMode(absl::string_view mode_id) {
  if (mode_id.empty()) {
    LOG(ERROR) << "mode_id is initialized.";
    return mozc::commands::DIRECT;
  }
  DLOG(INFO) << mode_id;

  // The information for ID names was available at
  // /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/System/Library/Frameworks/
  // Carbon.framework/Versions/A/Frameworks/HIToolbox.framework/Versions/A/Headers/TextServices.h
  // These IDs are also defined in Info.plist.
  if (mode_id == kRomanModeId) {
    return mozc::commands::HALF_ASCII;
  }
  if (mode_id == kKatakanaModeId) {
    return mozc::commands::FULL_KATAKANA;
  }
  if (mode_id == kHalfWidthKanaModeId) {
    return mozc::commands::HALF_KATAKANA;
  }
  if (mode_id == kFullWidthRomanModeId) {
    return mozc::commands::FULL_ASCII;
  }
  if (mode_id == kHiraganaModeId) {
    return mozc::commands::HIRAGANA;
  }

  LOG(ERROR) << "The code should not reach here.";
  return mozc::commands::DIRECT;
}

absl::string_view GetModeId(CompositionMode mode) {
  switch (mode) {
    case mozc::commands::DIRECT:
    case mozc::commands::HALF_ASCII:
      return kRomanModeId;
    case mozc::commands::FULL_KATAKANA:
      return kKatakanaModeId;
    case mozc::commands::HALF_KATAKANA:
      return kHalfWidthKanaModeId;
    case mozc::commands::FULL_ASCII:
      return kFullWidthRomanModeId;
    case mozc::commands::HIRAGANA:
      return kHiraganaModeId;
    default:
      LOG(ERROR) << "The code should not reach here.";
      return kRomanModeId;
  }
}

bool CanOpenLink(absl::string_view bundle_id) {
  // Should not open links during screensaver.
  return bundle_id != "com.apple.securityagent";
}

bool CanSelectedRange(absl::string_view bundle_id) {
  // Do not call selectedRange: method for the following
  // applications because it could lead to application crash.
  const bool is_supported = bundle_id != "com.microsoft.Excel" &&
                            bundle_id != "com.microsoft.Powerpoint" &&
                            bundle_id != "com.microsoft.Word";
  return is_supported;
}

bool CanDisplayModeSwitch(absl::string_view bundle_id) {
  // Do not call selectInputMode: method for the following
  // applications because it could cause some unexpected behavior.
  // MS-Word: When the display mode goes to ASCII but there is no
  // compositions, it goes to direct input mode instead of Half-ASCII
  // mode.  When the first composition character is alphanumeric (such
  // like pressing Shift-A at first), that character is directly
  // inserted into application instead of composition starting "A".
  return bundle_id != "com.microsoft.Word";
}

bool CanSurroundingText(absl::string_view bundle_id) {
  // Disables the surrounding text feature for the following application
  // because calling attributedSubstringFromRange to it is very heavy.
  return bundle_id != "com.evernote.Evernote";
}

bool HasLiveConversionReading(const Output &output) {
  return output.has_preedit() &&
         (output.live_conversion() || output.pre_live_conversion_reading());
}

bool ShouldDisplayRendererForOutput(const Output &output,
                                    bool show_conversion_status_indicator) {
  const bool status_visible = show_conversion_status_indicator &&
      output.has_preedit() && output.preedit().segment_size() > 0;
  return (output.has_candidate_window() &&
          output.candidate_window().candidate_size() > 0) ||
         HasLiveConversionReading(output) || status_visible;
}

bool ShouldSuppressCandidateWindowForLiveConversion(
    const Output &output, bool use_live_conversion) {
  if (!use_live_conversion ||
      !output.has_candidate_window() ||
      output.candidate_window().candidate_size() == 0 ||
      output.live_conversion()) {
    return false;
  }

  const mozc::commands::CandidateWindow &candidate_window =
      output.candidate_window();

  // focused_index is the protocol-level distinction between an actual
  // candidate-selection state and a passive suggestion state.  Do not infer
  // this state from the physical key or require a particular category.
  return !candidate_window.has_focused_index();
}

bool ShouldRecalculateRendererPosition(const RendererCommand &command,
                                       bool show_conversion_status_indicator) {
  return !command.visible() || (command.has_output() &&
      (HasLiveConversionReading(command.output()) ||
       (show_conversion_status_indicator &&
        command.output().has_preedit())));
}

int32_t GetRendererAnchorPosition(const Output &output) {
  // CandidateWindow::position is the protocol-defined position within the
  // composition.  Live conversion must not replace it with cursor - 1,
  // because doing so moves a suggestion toward the end of a multi-character
  // preedit.
  if (output.has_candidate_window()) {
    return output.candidate_window().position();
  }

  // A live-conversion ruby window can be displayed without a candidate
  // window.  Such a window is anchored to the start of the composition.
  return 0;
}

void SetRendererRectangle(
    const NSRect &line_rect, const NSPoint &baseline,
    RendererCommand::Rectangle *rectangle) {
  const int right_offset = line_rect.size.width;
  const int top_offset = -line_rect.size.height;

  rectangle->set_left(baseline.x);
  rectangle->set_right(baseline.x + right_offset);
  rectangle->set_top(baseline.y + top_offset);
  rectangle->set_bottom(baseline.y);
}

NSUInteger CodePointOffsetToUtf16Offset(NSString *text, uint32_t code_point_offset) {
  const NSUInteger length = [text length];
  NSUInteger utf16_offset = 0;
  uint32_t current_code_point = 0;
  while (utf16_offset < length && current_code_point < code_point_offset) {
    const unichar first = [text characterAtIndex:utf16_offset];
    ++utf16_offset;
    if (first >= 0xD800 && first <= 0xDBFF && utf16_offset < length) {
      const unichar second = [text characterAtIndex:utf16_offset];
      if (second >= 0xDC00 && second <= 0xDFFF) {
        ++utf16_offset;
      }
    }
    ++current_code_point;
  }
  return utf16_offset;
}
bool IsHighSurrogate(unichar value) {
  return value >= 0xD800 && value <= 0xDBFF;
}

bool IsLowSurrogate(unichar value) {
  return value >= 0xDC00 && value <= 0xDFFF;
}

NSString *TrimIncompleteZenzSurrogateEdges(NSString *text) {
  if (text == nil) {
    return nil;
  }

  NSUInteger begin = 0;
  NSUInteger end = [text length];

  if (begin < end && IsLowSurrogate([text characterAtIndex:begin])) {
    ++begin;
  }
  if (begin < end && IsHighSurrogate([text characterAtIndex:end - 1])) {
    --end;
  }

  if (begin == 0 && end == [text length]) {
    return text;
  }
  return [text substringWithRange:NSMakeRange(begin, end - begin)];
}

}  // namespace

@implementation MozcImkInputController
#pragma mark accessors for testing
@synthesize keyCodeMap = keyCodeMap_;
@synthesize yenSignCharacter = yenSignCharacter_;
@synthesize mode = mode_;
@synthesize rendererCommand = rendererCommand_;
@synthesize replacementRange = replacementRange_;
@synthesize imkClientForTest = imkClientForTest_;
@synthesize useLiveConversionForTest = useLiveConversion_;
@synthesize useZenzContextAcquisitionForTest = useZenzContextAcquisition_;
@synthesize secureEventInputStateForTest = secureEventInputStateForTest_;
- (mozc::client::ClientInterface *)mozcClient {
  return mozcClient_.get();
}
- (void)setMozcClient:(std::unique_ptr<mozc::client::ClientInterface>)newMozcClient {
  mozcClient_ = std::move(newMozcClient);
}
- (mozc::renderer::RendererInterface *)renderer {
  return mozcRenderer_.get();
}
- (void)setRenderer:(std::unique_ptr<mozc::renderer::RendererInterface>)newRenderer {
  mozcRenderer_ = std::move(newRenderer);
}
- (uint64_t)grimodexFocusEpochForTest {
  return grimodexFocusEpoch_;
}

#pragma mark object init/dealloc
// Initializer designated in IMKInputController. see:
// https://developer.apple.com/documentation/inputmethodkit/imkinputcontroller?language=objc

- (id)initWithServer:(IMKServer *)server delegate:(id)delegate client:(id)inputClient {
  // If server is nil, we are in the unit test environment.
  if (server == nil) {
    self = [super init];
    imkClientForTest_ = inputClient;
  } else {
    self = [super initWithServer:server delegate:delegate client:inputClient];
    imkClientForTest_ = nil;
  }
  if (!self) {
    return self;
  }
  keyCodeMap_ = [[KeyCodeMap alloc] init];
  replacementRange_ = NSMakeRange(NSNotFound, 0);
  originalString_ = [[NSMutableString alloc] init];
  composedString_ = [[NSMutableAttributedString alloc] init];
  cursorPosition_ = -1;
  useUnspecifiedSelectionRange_ = false;
  mode_ = mozc::commands::DIRECT;
  suppressSuggestion_ = false;
  yenSignCharacter_ = mozc::config::Config::YEN_SIGN;
  hasWritingDirection_ = false;
  verticalWriting_ = false;
  liveConversionAnchorLeft_ = 0;
  hasLiveConversionAnchorLeft_ = false;
  useLiveConversion_ = false;
  useConversionStatusIndicator_ = false;
  useZenzContextAcquisition_ = false;
  secureEventInputStateForTest_ = (server == nil) ? 0 : -1;
  grimodexFocusEpoch_ = mozc::grimodex::AdvanceFocusEpoch(0);
  grimodexSecureInput_ = false;
  grimodexSecureInputInitialized_ = false;
  // Unit tests directly exercise the controller without IMK activation.
  // Production controllers become active only in activateServer:.
  grimodexActive_ = server == nil;
  delayedSessionCommandFocusEpoch_ = 0;
  rendererCommandFocusEpoch_ = 0;
  rendererCallbackToken_ = 0;
  // Unit tests inject mock renderer/client objects immediately after
  // construction. Avoid creating production process/IPC dependencies in the
  // server == nil test environment.
  if (server != nil) {
    mozcRenderer_ = mozc::renderer::RendererClient::Create();
    mozcClient_ = mozc::client::ClientFactory::NewClient();
  }
  lastKeyDownTime_ = 0;
  lastKeyCode_ = 0;

  // We don't check the return value of NSBundle because it fails during tests.
  [[NSBundle mainBundle] loadNibNamed:@"Config" owner:self topLevelObjects:nil];
  const bool hasRuntimeDependencies =
      server == nil || (mozcRenderer_ != nullptr && mozcClient_ != nullptr);
  if (!originalString_ || !composedString_ || !hasRuntimeDependencies) {
    self = nil;
  } else {
    DLOG(INFO) << [[NSString
        stringWithFormat:@"initWithServer: %@ %@ %@", server, delegate, inputClient] UTF8String];
    if (mozcRenderer_ && !mozcRenderer_->Activate()) {
      LOG(ERROR) << "Cannot activate renderer";
      mozcRenderer_.reset();
    }
    [self setupClientBundle:inputClient];
    if (mozcClient_) {
      [self setupCapability];
    }
    RendererCommand::ApplicationInfo *applicationInfo = rendererCommand_.mutable_application_info();
    applicationInfo->set_process_id(::getpid());
    // thread_id and receiver_handle are not used currently in Mac but
    // set some values to prevent warning.
    applicationInfo->set_thread_id(0);
    applicationInfo->set_receiver_handle(0);
  }

  return self;
}

- (void)dealloc {
  [gRendererReceiver clearCurrentController:self];
  grimodexActive_ = false;
  [self cancelDelayedSessionCommand];
  keyCodeMap_ = nil;
  originalString_ = nil;
  composedString_ = nil;
  imkClientForTest_ = nil;
  mozcRenderer_.reset();
  mozcClient_.reset();
  DLOG(INFO) << "dealloc server";
}

- (id)client {
  if (imkClientForTest_) {
    return imkClientForTest_;
  }
  return [super client];
}

- (NSMenu *)menu {
  return menu_;
}

#pragma mark IMKStateSetting Protocol
// Currently it just ignores the following methods:
//   Modes, showPreferences, valueForTag
// They are described at
// https://developer.apple.com/documentation/inputmethodkit/imkstatesetting?language=objc

- (void)activateServer:(id)sender {
  if (imkClientForTest_) {
    return;
  }
  [super activateServer:sender];
  grimodexActive_ = true;
  const uint64_t epochBeforeBundleRefresh = grimodexFocusEpoch_;
  [self setupClientBundle:sender];
  // Each activation is a fresh input domain even when the same application
  // remains frontmost.  Carry the new domain on RESET_CONTEXT so server-side
  // project state cannot bleed across focus transitions.
  if (grimodexFocusEpoch_ == epochBeforeBundleRefresh) {
    [self advanceGrimodexFocusEpoch];
  }
  [self resetContextForClient:sender];
  if (rendererCommand_.visible() && mozcRenderer_) {
    mozcRenderer_->ExecCommand(rendererCommand_);
  }
  [self handleConfig];

  // Sets this controller as the active controller to receive messages from the renderer process.
  [gRendererReceiver setCurrentController:self];

  std::string window_name, window_owner;
  if (mozc::MacUtil::GetFrontmostWindowNameAndOwner(&window_name, &window_owner)) {
    DLOG(INFO) << "frontmost window name: \"" << window_name << "\" " << "owner: \"" << window_owner
               << "\"";
    suppressSuggestion_ = mozc::MacUtil::IsSuppressSuggestionWindow(window_name, window_owner);
  }

  DLOG(INFO) << kProductNameInEnglish << " client (" << self << "): activated for " << sender;
  DLOG(INFO) << "sender bundleID: " << clientBundle_;
}

- (void)deactivateServer:(id)sender {
  // Revoke renderer and server outputs before touching the host or invoking
  // the superclass.  Advancing the epoch invalidates every synchronous or
  // delayed result captured for this activation.
  [gRendererReceiver clearCurrentController:self];
  grimodexActive_ = false;
  [self advanceGrimodexFocusEpoch];

  if (imkClientForTest_) {
    return;
  }

  RendererCommand clearCommand;
  clearCommand.set_type(RendererCommand::UPDATE);
  clearCommand.set_visible(false);
  clearCommand.clear_output();
  if (mozcRenderer_) {
    mozcRenderer_->ExecCommand(clearCommand);
  }
  DLOG(INFO) << kProductNameInEnglish << " client (" << self << "): deactivated";
  DLOG(INFO) << "sender bundleID: " << clientBundle_;
  [super deactivateServer:sender];
}

- (NSUInteger)recognizedEvents:(id)sender {
  // Because we want to handle single Shift key pressing later, now I
  // turned on NSFlagsChanged also.
  return NSEventMaskKeyDown | NSEventMaskFlagsChanged;
}

// This method is called when a user changes the input mode.
- (void)setValue:(id)value forTag:(long)tag client:(id)sender {
  if (imkClientForTest_) {
    return;
  }

  CompositionMode new_mode = [value isKindOfClass:[NSString class]]
                                 ? GetCompositionMode([value UTF8String])
                                 : mozc::commands::DIRECT;
  if (new_mode == mozc::commands::HALF_ASCII && [composedString_ length] == 0) {
    new_mode = mozc::commands::DIRECT;
  }

  [self switchMode:new_mode client:sender];
  [self handleConfig];
  [super setValue:value forTag:tag client:sender];
}

#pragma mark internal methods

- (void)handleConfig {
  // Get the config and set client-side behaviors
  Config config;
  if (!mozcClient_->GetConfig(&config)) {
    LOG(ERROR) << "Cannot obtain the current config";
    return;
  }

  InputMode input_mode = ASCII;
  if (config.preedit_method() == Config::KANA) {
    input_mode = KANA;
  }
  [keyCodeMap_ setInputMode:input_mode];
  yenSignCharacter_ = config.yen_sign_character();
  useLiveConversion_ = config.use_live_conversion();
  useConversionStatusIndicator_ = config.show_conversion_status_indicator();
  // Avoid an extra TEST_SEND_KEY round trip for ordinary Mozc input.
  useZenzContextAcquisition_ =
      useLiveConversion_ && config.use_zenz_live_correction();

  if (config.use_japanese_layout()) {
    // Apple does not have "Japanese" layout actually -- here sets
    // "US" layout, which means US-ASCII layout or JIS layout
    // depending on which type of keyboard is actually connected.
    [[self client] overrideKeyboardWithKeyboardNamed:@"com.apple.keylayout.US"];
  }
}

- (void)setupClientBundle:(id)sender {
  std::string newClientBundle;
  NSString *bundleIdentifier = [sender bundleIdentifier];
  if (bundleIdentifier != nil && [bundleIdentifier length] > 0) {
    newClientBundle.assign([bundleIdentifier UTF8String]);
  }
  if (newClientBundle != clientBundle_) {
    clientBundle_ = std::move(newClientBundle);
    [self advanceGrimodexFocusEpoch];
  }
}

- (void)advanceGrimodexFocusEpoch {
  grimodexFocusEpoch_ = mozc::grimodex::AdvanceFocusEpoch(grimodexFocusEpoch_);
  [self cancelDelayedSessionCommand];
  rendererCommandFocusEpoch_ = 0;
  rendererCallbackToken_ = 0;
  // Composition and replacement ranges belong to the input domain that
  // created them.  Keeping either across an application/focus/security
  // transition could make a later result mutate text in the new domain.
  replacementRange_ = NSMakeRange(NSNotFound, 0);
  [composedString_ deleteCharactersInRange:
                       NSMakeRange(0, [composedString_ length])];
  [originalString_ setString:@""];
  cursorPosition_ = -1;
  useUnspecifiedSelectionRange_ = false;
  [self resetWritingDirection];
  [NSObject cancelPreviousPerformRequestsWithTarget:self
                                           selector:@selector(delayedUpdateCandidates)
                                             object:nil];
  [self clearCandidates];
}

- (BOOL)refreshSecureInputState {
  // This check intentionally happens before every path that may query the
  // client's text.  The test override keeps that security contract directly
  // verifiable without depending on process-global Carbon state.
  const bool secureInput = [self isSecureEventInputEnabledForContext];
  const bool changed =
      grimodexSecureInputInitialized_ && secureInput != grimodexSecureInput_;
  const bool initiallySecure =
      !grimodexSecureInputInitialized_ && secureInput != grimodexSecureInput_;
  grimodexSecureInputInitialized_ = true;
  grimodexSecureInput_ = secureInput;
  if (changed || initiallySecure) {
    [self advanceGrimodexFocusEpoch];
  }
  return secureInput;
}

- (BOOL)isCurrentGrimodexEpoch:(uint64_t)focusEpoch {
  [self refreshSecureInputState];
  return grimodexActive_ && focusEpoch != 0 &&
         focusEpoch == grimodexFocusEpoch_;
}

- (BOOL)isCurrentNonSecureTextQueryEpoch:(uint64_t)focusEpoch {
  return [self isCurrentGrimodexEpoch:focusEpoch] &&
         !grimodexSecureInput_;
}

- (mozc::commands::Context)buildClientContextForClient:(id<IMKTextInput>)client
                               includeSurroundingText:(BOOL)includeSurroundingText {
  const bool secureInput = [self refreshSecureInputState];
  const uint64_t focusEpoch = grimodexFocusEpoch_;
  mozc::grimodex::SurroundingTextProvider surroundingTextProvider;
  if (!secureInput && includeSurroundingText && client != nil &&
      [composedString_ length] == 0 && CanSelectedRange(clientBundle_) &&
      CanSurroundingText(clientBundle_)) {
    surroundingTextProvider = [self, client]()
        -> std::optional<mozc::grimodex::SurroundingText> {
      mozc::commands::Context surroundingContext;
      if (![self collectSurroundingContext:&surroundingContext client:client]) {
        return std::nullopt;
      }
      mozc::grimodex::SurroundingText surroundingText;
      surroundingText.preceding_text = surroundingContext.preceding_text();
      surroundingText.following_text = surroundingContext.following_text();
      return surroundingText;
    };
  }
  mozc::commands::Context context = mozc::grimodex::BuildClientContext(
      clientBundle_, "imkit", secureInput, focusEpoch, surroundingTextProvider);
  // A Secure Event Input transition can race a non-secure text query.  The
  // collector rechecks Carbon before every client API; rebuild from the new
  // state if one of those checks advanced the domain while the lazy provider
  // was running.
  if (focusEpoch != grimodexFocusEpoch_ ||
      secureInput != grimodexSecureInput_) {
    context = mozc::grimodex::BuildClientContext(
        clientBundle_, "imkit", grimodexSecureInput_, grimodexFocusEpoch_);
  }
  return context;
}

- (void)resetContextForClient:(id<IMKTextInput>)client {
  SessionCommand command;
  command.set_type(SessionCommand::RESET_CONTEXT);
  Output output;
  const mozc::commands::Context context =
      [self buildClientContextForClient:client includeSurroundingText:NO];
  mozcClient_->SendCommandWithContext(command, context, &output);
}

- (void)setupCapability {
  Capability capability;

  if (CanSelectedRange(clientBundle_)) {
    capability.set_text_deletion(Capability::DELETE_PRECEDING_TEXT);
  } else {
    capability.set_text_deletion(Capability::NO_TEXT_DELETION_CAPABILITY);
  }

  mozcClient_->set_client_capability(capability);
}

// Mode changes to direct and clean up the status.
- (void)switchModeToDirect:(id)sender {
  mode_ = mozc::commands::DIRECT;
  DLOG(INFO) << "Mode switch: HIRAGANA, KATAKANA, etc. -> DIRECT";
  KeyEvent keyEvent;
  Output output;
  keyEvent.set_special_key(mozc::commands::KeyEvent::OFF);
  const mozc::commands::Context context =
      [self buildClientContextForClient:sender includeSurroundingText:NO];
  const uint64_t sendingFocusEpoch = context.grimodex().focus_epoch();
  mozcClient_->SendKeyWithContext(keyEvent, context, &output);
  if (output.has_result() &&
      [self isCurrentGrimodexEpoch:sendingFocusEpoch]) {
    [self commitText:output.result().value().c_str() client:sender];
  }
  if ([composedString_ length] > 0) {
    [self updateComposedString:nullptr];
    [self clearCandidates];
  }
}

- (void)switchMode:(CompositionMode)new_mode client:(id)sender {
  if (mode_ == new_mode) {
    return;
  }

  if (new_mode == mozc::commands::DIRECT) {
    // Turn off the IME and commit the composing text.
    DLOG(INFO) << "Mode switch: HIRAGANA, KATAKANA, etc. -> DIRECT";
    KeyEvent keyEvent;
    Output output;
    keyEvent.set_special_key(mozc::commands::KeyEvent::OFF);
    const mozc::commands::Context context =
        [self buildClientContextForClient:sender includeSurroundingText:NO];
    const uint64_t sendingFocusEpoch = context.grimodex().focus_epoch();
    mozcClient_->SendKeyWithContext(keyEvent, context, &output);
    if (output.has_result() &&
        [self isCurrentGrimodexEpoch:sendingFocusEpoch]) {
      [self commitText:output.result().value().c_str() client:sender];
    }
    if ([composedString_ length] > 0) {
      [self updateComposedString:nullptr];
      [self clearCandidates];
    }
    mode_ = mozc::commands::DIRECT;
    return;
  }

  if (mode_ == mozc::commands::DIRECT) {
    // Turn on the IME as the mode is changed from DIRECT to an active mode.
    DLOG(INFO) << "Mode switch: DIRECT -> HIRAGANA, KATAKANA, etc.";
    KeyEvent keyEvent;
    Output output;
    keyEvent.set_special_key(mozc::commands::KeyEvent::ON);
    const mozc::commands::Context context =
        [self buildClientContextForClient:sender includeSurroundingText:NO];
    mozcClient_->SendKeyWithContext(keyEvent, context, &output);
  }

  // Switch composition mode.
  DLOG(INFO) << "Switch composition mode.";
  SessionCommand command;
  command.set_type(mozc::commands::SessionCommand::SWITCH_COMPOSITION_MODE);
  command.set_composition_mode(new_mode);
  Output output;
  const mozc::commands::Context context =
      [self buildClientContextForClient:sender includeSurroundingText:NO];
  mozcClient_->SendCommandWithContext(command, context, &output);
  mode_ = new_mode;
}

- (void)switchDisplayMode {
  if (!CanDisplayModeSwitch(clientBundle_)) {
    return;
  }

  absl::string_view mode_id = GetModeId(mode_);
  [[self client] selectInputMode:[NSString stringWithUTF8String:mode_id.data()]];
}

- (void)commitText:(const char *)text client:(id)sender {
  if (text == nullptr) {
    return;
  }

  [sender insertText:[NSString stringWithUTF8String:text] replacementRange:replacementRange_];
  replacementRange_ = NSMakeRange(NSNotFound, 0);
}

- (void)launchWordRegisterTool:(id)client {
  ::setenv(mozc::kWordRegisterEnvironmentName, "", 1);
  // Never inspect the host application's text while Secure Event Input is
  // active, even for an explicitly requested tool launch.
  const bool secureInput = [self refreshSecureInputState];
  const uint64_t textQueryEpoch = grimodexFocusEpoch_;
  if (grimodexActive_ && !secureInput && CanSelectedRange(clientBundle_)) {
    NSRange selectedRange = [client selectedRange];
    if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
      MacProcess::LaunchMozcTool("word_register_dialog");
      return;
    }
    if (selectedRange.location != NSNotFound && selectedRange.length != NSNotFound &&
        selectedRange.length > 0) {
      NSAttributedString *attributedText =
          [client attributedSubstringFromRange:selectedRange];
      if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
        MacProcess::LaunchMozcTool("word_register_dialog");
        return;
      }
      NSString *text = [attributedText string];
      if ([self isCurrentNonSecureTextQueryEpoch:textQueryEpoch] && text != nil) {
        ::setenv(mozc::kWordRegisterEnvironmentName, [text UTF8String], 1);
      }
    }
  }
  MacProcess::LaunchMozcTool("word_register_dialog");
}

- (void)invokeReconvert:(const SessionCommand *)command client:(id)sender {
  if ([self refreshSecureInputState] || !CanSelectedRange(clientBundle_)) {
    return;
  }
  const uint64_t textQueryEpoch = grimodexFocusEpoch_;

  NSRange selectedRange = [sender selectedRange];
  if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
    return;
  }
  if (selectedRange.location == NSNotFound || selectedRange.length == NSNotFound) {
    // the application does not support reconversion.
    return;
  }

  DLOG(INFO) << selectedRange.location << ", " << selectedRange.length;
  SessionCommand sending_command;
  Output output;
  sending_command = *command;

  if (selectedRange.length == 0) {
    // Currently no range is selected for reconversion.  Tries to
    // invoke UNDO instead.
    [self invokeUndo:sender];
    return;
  }

  if (!sending_command.has_text()) {
    NSAttributedString *attributedText =
        [sender attributedSubstringFromRange:selectedRange];
    if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
      return;
    }
    NSString *text = [attributedText string];
    if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
      return;
    }
    if (!text) {
      return;
    }
    sending_command.set_text([text UTF8String]);
  }

  const mozc::commands::Context context =
      [self buildClientContextForClient:sender includeSurroundingText:NO];
  const uint64_t sendingFocusEpoch = context.grimodex().focus_epoch();
  if (sendingFocusEpoch != textQueryEpoch) {
    return;
  }
  if (mozcClient_->SendCommandWithContext(sending_command, context, &output)) {
    if ([self isCurrentGrimodexEpoch:sendingFocusEpoch]) {
      replacementRange_ = selectedRange;
    }
    [self processOutput:&output
                   client:sender
        sendingFocusEpoch:sendingFocusEpoch];
  }
}

- (void)invokeUndo:(id)sender {
  if ([self refreshSecureInputState] || !CanSelectedRange(clientBundle_)) {
    return;
  }
  const uint64_t textQueryEpoch = grimodexFocusEpoch_;

  NSRange selectedRange = [sender selectedRange];
  if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
    return;
  }
  if (selectedRange.location == NSNotFound || selectedRange.length == NSNotFound ||
      // Some applications such like iTunes does not return NSNotFound
      // range but (0, 0).  However, the range starting with negative
      // location has to be invalid, then we can reject such apps.
      selectedRange.location == 0) {
    return;
  }

  DLOG(INFO) << selectedRange.location << ", " << selectedRange.length;
  SessionCommand command;
  Output output;
  command.set_type(SessionCommand::UNDO);
  const mozc::commands::Context context =
      [self buildClientContextForClient:sender includeSurroundingText:NO];
  const uint64_t sendingFocusEpoch = context.grimodex().focus_epoch();
  if (sendingFocusEpoch != textQueryEpoch) {
    return;
  }
  if (mozcClient_->SendCommandWithContext(command, context, &output)) {
    [self processOutput:&output
                   client:sender
        sendingFocusEpoch:sendingFocusEpoch];
  }
}

- (void)processOutput:(const mozc::commands::Output *)output client:(id)sender {
  [self refreshSecureInputState];
  [self processOutput:output
                 client:sender
      sendingFocusEpoch:grimodexFocusEpoch_];
}

- (void)processOutput:(const mozc::commands::Output *)output
                 client:(id)sender
      sendingFocusEpoch:(uint64_t)sendingFocusEpoch {
  if (output == nullptr) {
    return;
  }
  if (!output->consumed()) {
    return;
  }

  // A server round trip can reenter the host run loop.  Validate the domain
  // that sent the request before the output is logged or allowed to mutate
  // host text, preedit state, or candidate UI.
  if (![self isCurrentGrimodexEpoch:sendingFocusEpoch]) {
    return;
  }
  const bool secureInput = grimodexSecureInput_;

  if (!secureInput) {
    DLOG(INFO) << output->Utf8DebugString();
  }
  if (output->has_url()) {
    if (![self isCurrentGrimodexEpoch:sendingFocusEpoch]) {
      return;
    }
    NSString *url = [NSString stringWithUTF8String:output->url().c_str()];
    [self openLink:[NSURL URLWithString:url]];
  }

  if (output->has_result()) {
    if (![self isCurrentGrimodexEpoch:sendingFocusEpoch]) {
      return;
    }
    [self commitText:output->result().value().c_str() client:sender];
  }

  // Handles deletion range.  We do not even handle it for some
  // applications to prevent application crashes.
  if (!secureInput && output->has_deletion_range() &&
      CanSelectedRange(clientBundle_)) {
    if ([composedString_ length] == 0 && replacementRange_.location == NSNotFound &&
        [self isCurrentNonSecureTextQueryEpoch:sendingFocusEpoch]) {
      NSRange selectedRange = [sender selectedRange];
      if (![self isCurrentNonSecureTextQueryEpoch:sendingFocusEpoch]) {
        return;
      }
      const mozc::commands::DeletionRange &deletion_range = output->deletion_range();
      const NSUInteger maxSignedRange =
          static_cast<NSUInteger>(std::numeric_limits<int64_t>::max());
      if (selectedRange.location != NSNotFound &&
          selectedRange.length != NSNotFound &&
          selectedRange.location <= maxSignedRange &&
          selectedRange.length <= maxSignedRange) {
        const int64_t location = static_cast<int64_t>(selectedRange.location);
        const int64_t length = static_cast<int64_t>(selectedRange.length);
        const int64_t offset = deletion_range.offset();
        const int64_t lengthDelta = deletion_range.length();
        // DeletionRange offsets are non-positive and lengths are
        // non-negative. Reject malformed or overflowing server output rather
        // than allowing NSUInteger arithmetic to wrap.
        if (offset <= 0 && lengthDelta >= 0 && location >= -offset &&
            length <= std::numeric_limits<int64_t>::max() - lengthDelta) {
          selectedRange.location =
              static_cast<NSUInteger>(location + offset);
          selectedRange.length =
              static_cast<NSUInteger>(length + lengthDelta);
          replacementRange_ = selectedRange;
        }
      }
    } else {
      // We have to consider the case that there is already
      // composition and/or we already set the position of the
      // composition by replacementRange_.  We do nothing here at this
      // time because we already found that it will involve several
      // buggy behaviors with Carbon apps and MS Office.
      // TODO(mukai): find the right behavior.
    }
  }

  if (![self isCurrentGrimodexEpoch:sendingFocusEpoch]) {
    return;
  }
  if (![self updateComposedString:&(output->preedit())
                   liveConversion:output->live_conversion()
                sendingFocusEpoch:sendingFocusEpoch]) {
    return;
  }
  [self updateCandidates:output];

  if (output->has_mode()) {
    if (![self isCurrentGrimodexEpoch:sendingFocusEpoch]) {
      return;
    }
    CompositionMode new_mode = output->mode();
    // Do not allow HALF_ASCII with empty composition.  This should be
    // handled in the converter, but just in case.
    if (new_mode == mozc::commands::HALF_ASCII &&
        (!output->has_preedit() || output->preedit().segment_size() == 0)) {
      new_mode = mozc::commands::DIRECT;
      [self switchMode:new_mode client:sender];
    }
    if (new_mode != mode_) {
      mode_ = new_mode;
      [self switchDisplayMode];
    }
  }

  if (output->has_launch_tool_mode()) {
    if (![self isCurrentGrimodexEpoch:sendingFocusEpoch]) {
      return;
    }
    switch (output->launch_tool_mode()) {
      case mozc::commands::Output::CONFIG_DIALOG:
        MacProcess::LaunchMozcTool("config_dialog");
        break;
      case mozc::commands::Output::DICTIONARY_TOOL:
        MacProcess::LaunchMozcTool("dictionary_tool");
        break;
      case mozc::commands::Output::WORD_REGISTER_DIALOG:
        [self launchWordRegisterTool:sender];
        break;
      default:
        // do nothing
        break;
    }
  }

  // Handle callbacks.
  if (output->has_callback() && output->callback().has_session_command()) {
    if (![self isCurrentGrimodexEpoch:sendingFocusEpoch]) {
      return;
    }
    const Output::Callback &callback = output->callback();
    const SessionCommand &callback_command = callback.session_command();
    if (callback.has_delay_millisec() && callback.delay_millisec() > 0) {
      [self cancelDelayedSessionCommand];
      delayedSessionCommand_ = std::make_unique<SessionCommand>(callback_command);
      delayedSessionCommandClient_ = sender;
      delayedSessionCommandFocusEpoch_ = grimodexFocusEpoch_;
      const NSTimeInterval delay =
          static_cast<NSTimeInterval>(callback.delay_millisec()) / 1000.0;
      delayedSessionCommandTimer_ =
          [NSTimer scheduledTimerWithTimeInterval:delay
                                           target:self
                                         selector:@selector(delayedInvokeSessionCommandFromTimer:)
                                         userInfo:nil
                                          repeats:NO];
    } else {
      [self dispatchSessionCommand:&callback_command client:sender];
    }
  }
}

- (void)cancelDelayedSessionCommand {
  [delayedSessionCommandTimer_ invalidate];
  delayedSessionCommandTimer_ = nil;
  delayedSessionCommand_.reset();
  delayedSessionCommandClient_ = nil;
  delayedSessionCommandFocusEpoch_ = 0;
}

- (void)delayedInvokeSessionCommandFromTimer:(NSTimer *)timer {
  if (timer != delayedSessionCommandTimer_) {
    return;
  }
  if (!delayedSessionCommand_) {
    return;
  }
  const uint64_t scheduledEpoch = delayedSessionCommandFocusEpoch_;
  [self refreshSecureInputState];
  if (timer != delayedSessionCommandTimer_ || !delayedSessionCommand_) {
    return;
  }
  if (scheduledEpoch == 0 || scheduledEpoch != grimodexFocusEpoch_) {
    [self cancelDelayedSessionCommand];
    return;
  }

  SessionCommand command = *delayedSessionCommand_;
  id sender = delayedSessionCommandClient_;
  [self cancelDelayedSessionCommand];
  [self dispatchSessionCommand:&command client:sender];
}

- (void)dispatchSessionCommand:(const SessionCommand *)command client:(id)sender {
  if (command->type() == SessionCommand::CONVERT_REVERSE) {
    [self invokeReconvert:command client:sender];
  } else if (command->type() == SessionCommand::UNDO) {
    [self invokeUndo:sender];
  } else {
    Output output_for_callback;
    const mozc::commands::Context context =
        [self buildClientContextForClient:sender includeSurroundingText:NO];
    const uint64_t sendingFocusEpoch = context.grimodex().focus_epoch();
    if (mozcClient_->SendCommandWithContext(*command, context,
                                            &output_for_callback)) {
      [self processOutput:&output_for_callback
                     client:sender
          sendingFocusEpoch:sendingFocusEpoch];
    }
  }
}

#pragma mark Mozc Server methods

#pragma mark IMKServerInput Protocol
// Currently MozcImkInputController uses handleEvent:client:
// method to handle key events.  It does not support inputText:client:
// nor inputText:key:modifiers:client:.
// Because MozcImkInputController does not use IMKCandidates,
// the following methods are not needed to implement:
//   candidates
//
// The meaning of these methods are described at:
// https://developer.apple.com/documentation/inputmethodkit/imkserverinput?language=objc

- (id)originalString:(id)sender {
  return originalString_;
}

- (void)updateComposedString:(const Preedit *)preedit {
  [self updateComposedString:preedit liveConversion:false];
}

- (void)updateComposedString:(const Preedit *)preedit
              liveConversion:(bool)live_conversion {
  [self refreshSecureInputState];
  [self updateComposedString:preedit
              liveConversion:live_conversion
           sendingFocusEpoch:grimodexFocusEpoch_];
}

- (BOOL)updateComposedString:(const Preedit *)preedit
              liveConversion:(bool)live_conversion
           sendingFocusEpoch:(uint64_t)sendingFocusEpoch {
  if (![self isCurrentGrimodexEpoch:sendingFocusEpoch]) {
    return NO;
  }
  // If the last and the current composed string length is 0,
  // we don't update the composition.
  if (([composedString_ length] == 0) &&
      ((preedit == nullptr || preedit->segment_size() == 0))) {
    // No composition owns the cached writing direction at this point.  Clear
    // it even though there is no marked-text update to send to the client.
    [self resetWritingDirection];
    return YES;
  }

  [composedString_ deleteCharactersInRange:NSMakeRange(0, [composedString_ length])];
  cursorPosition_ = -1;
  useUnspecifiedSelectionRange_ = false;
  if (preedit != nullptr) {
    NSDictionary *highlightAttributes =
        [self markForStyle:kTSMHiliteSelectedConvertedText
                   atRange:NSMakeRange(NSNotFound, 0)];
    NSDictionary *underlineAttributes =
        [self markForStyle:kTSMHiliteConvertedText
                   atRange:NSMakeRange(NSNotFound, 0)];

    for (size_t i = 0; i < preedit->segment_size(); ++i) {
      const Preedit::Segment &seg = preedit->segment(static_cast<int32_t>(i));
      NSDictionary *attributes = nil;
      if (!live_conversion) {
        attributes = (seg.annotation() == Preedit::Segment::HIGHLIGHT)
                         ? highlightAttributes
                         : underlineAttributes;
      }
      NSString *seg_string = [NSString stringWithUTF8String:seg.value().c_str()];
      NSAttributedString *seg_attributed_string =
          [[NSAttributedString alloc] initWithString:seg_string attributes:attributes];
      [composedString_ appendAttributedString:seg_attributed_string];
    }

    if (live_conversion && [composedString_ length] > 0) {
      // Live conversion is a conversion preview, not a focused-clause
      // selection.  Leaving the selection unspecified lets AppKit keep its
      // native insertion caret instead of showing Mozc's focused-clause
      // boundary as the caret.
      useUnspecifiedSelectionRange_ = true;
    } else {
      cursorPosition_ = static_cast<int>(
          CodePointOffsetToUtf16Offset([composedString_ string], preedit->cursor()));
    }
  }
  if ([composedString_ length] == 0) {
    [originalString_ setString:@""];
    replacementRange_ = NSMakeRange(NSNotFound, 0);
    [self resetWritingDirection];
  }

  // selectionRange may enter the host application and reenter the run loop.
  // In particular, Secure Event Input can become enabled there.  Revalidate
  // the sending domain before exposing the preedit to the host.  An epoch
  // transition also clears the local composition above.
  const NSRange selectionRange = [self selectionRange];
  if (![self isCurrentGrimodexEpoch:sendingFocusEpoch]) {
    return NO;
  }

  // Update the composed string of the client applications.
  [[self client] setMarkedText:composedString_
                selectionRange:selectionRange
              replacementRange:replacementRange_];
  return [self isCurrentGrimodexEpoch:sendingFocusEpoch];
}

- (void)commitComposition:(id)sender {
  if ([composedString_ length] == 0) {
    DLOG(INFO) << "Nothing is committed.";
    return;
  }
  [self commitText:[[composedString_ string] UTF8String] client:sender];

  SessionCommand command;
  Output output;
  command.set_type(SessionCommand::SUBMIT);
  const mozc::commands::Context context =
      [self buildClientContextForClient:sender includeSurroundingText:NO];
  mozcClient_->SendCommandWithContext(command, context, &output);
  [self clearCandidates];
  [self updateComposedString:nullptr];
}

- (id)composedString:(id)sender {
  return composedString_;
}


- (void)clearCandidates {
  hasLiveConversionAnchorLeft_ = false;
  rendererCommandFocusEpoch_ = 0;
  rendererCallbackToken_ = 0;
  rendererCommand_.set_type(RendererCommand::UPDATE);
  rendererCommand_.set_visible(false);
  rendererCommand_.clear_output();
  rendererCommand_.mutable_application_info()->clear_renderer_callback_token();
  if (mozcRenderer_) {
    mozcRenderer_->ExecCommand(rendererCommand_);
  }
}

// |selecrionRange| method is defined at IMKInputController class and
// means the position of cursor actually.
- (NSRange)selectionRange {
  // Refresh first: a Secure Event Input transition clears the composition and
  // with it the live-conversion selection state checked below.
  const bool secureInput = [self refreshSecureInputState];
  const uint64_t textQueryEpoch = grimodexFocusEpoch_;
  if (useUnspecifiedSelectionRange_) {
    return NSMakeRange(NSNotFound, 0);
  }
  if ((!grimodexActive_ || secureInput) && cursorPosition_ == -1) {
    return NSMakeRange(0, 0);
  }

  if (cursorPosition_ != -1) {
    return NSMakeRange(cursorPosition_, 0);
  }

  // Query the active IMK text client directly so the domain can be rechecked
  // after the potentially reentrant host call.
  id client = [self client];
  if (client == nil) {
    return NSMakeRange(0, 0);
  }
  const NSRange selectedRange = [client selectedRange];
  if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
    return NSMakeRange(0, 0);
  }
  return selectedRange;
}

- (void)resetWritingDirection {
  hasWritingDirection_ = false;
  verticalWriting_ = false;

  if (rendererCommand_.has_application_info() &&
      rendererCommand_.application_info().has_composition_target()) {
    rendererCommand_.mutable_application_info()
        ->mutable_composition_target()
        ->clear_vertical_writing();
  }
}

- (void)updateWritingDirectionFromAttributes:(NSDictionary *)attributes {
  // IMKTextOrientationName is returned by
  // attributesForCharacterIndex:lineHeightRectangle:.  Its NSNumber value is
  // YES for horizontal layout and NO for vertical layout.
  if (hasWritingDirection_ || attributes == nil) {
    return;
  }

  id orientation = attributes[IMKTextOrientationName];
  if (![orientation isKindOfClass:[NSNumber class]]) {
    return;
  }

  hasWritingDirection_ = true;
  verticalWriting_ = ![orientation boolValue];
  rendererCommand_.mutable_application_info()
      ->mutable_composition_target()
      ->set_vertical_writing(verticalWriting_);
}

- (void)delayedUpdateCandidates {
  if (!mozcRenderer_) {
    return;
  }

  // Secure Event Input must be checked before querying any client geometry.
  // Candidate output is also scoped to the domain that produced it.
  if ([self refreshSecureInputState] || rendererCommandFocusEpoch_ == 0 ||
      rendererCommandFocusEpoch_ != grimodexFocusEpoch_) {
    [self clearCandidates];
    return;
  }

  if (!rendererCommand_.has_output() ||
      !ShouldDisplayRendererForOutput(rendererCommand_.output(),
                                      useConversionStatusIndicator_)) {
    hasLiveConversionAnchorLeft_ = false;
    rendererCommand_.set_visible(false);
    mozcRenderer_->ExecCommand(rendererCommand_);
    return;
  }

  if (!HasLiveConversionReading(rendererCommand_.output())) {
    hasLiveConversionAnchorLeft_ = false;
  }

  // The candidate window position is not recalculated if the
  // candidate already appears on the screen.  Therefore, if a user
  // moves client application window by mouse, candidate window won't
  // follow the move of window.  This is done because:
  //  - some applications like Emacs or Google Chrome don't return the
  //    cursor position correctly.  The candidate window moves
  //    frequently with those application, which irritates users.
  //  - Kotoeri does this too.
  if (!ShouldRecalculateRendererPosition(rendererCommand_,
                                         useConversionStatusIndicator_)) {
    // Call ExecCommand anyway to update other information like candidate words.
    mozcRenderer_->ExecCommand(rendererCommand_);
    return;
  }

  rendererCommand_.set_visible(true);

  NSRect preeditRect = NSZeroRect;
  const uint64_t geometryEpoch = rendererCommandFocusEpoch_;
  // Snapshot what is needed from the output: a domain transition detected
  // after a host call clears |rendererCommand_|.
  const int32_t position = GetRendererAnchorPosition(rendererCommand_.output());
  const bool hasLiveConversionReading =
      HasLiveConversionReading(rendererCommand_.output());

  // Some applications throw an exception from
  // attributesForCharacterIndex:lineHeightRectangle:.
  DLOG(INFO) << "attributesForCharacterIndex: " << position;
  @try {
    NSDictionary *clientData =
        [[self client] attributesForCharacterIndex:position
                              lineHeightRectangle:&preeditRect];
    if (![self isCurrentNonSecureTextQueryEpoch:geometryEpoch]) {
      [self clearCandidates];
      return;
    }
    [self updateWritingDirectionFromAttributes:clientData];

    // IMKBaseline is the left-bottom coordinate of the requested character.
    const NSPoint baseline = [clientData[@"IMKBaseline"] pointValue];

    int candidate_left = baseline.x;
    if (hasLiveConversionReading) {
      if (!hasLiveConversionAnchorLeft_) {
        liveConversionAnchorLeft_ = baseline.x;
        hasLiveConversionAnchorLeft_ = true;
      }
      candidate_left = liveConversionAnchorLeft_;
    }

    SetRendererRectangle(
        preeditRect, NSMakePoint(candidate_left, baseline.y),
        rendererCommand_.mutable_preedit_rectangle());

    rendererCommand_.clear_ruby_preedit_rectangle();

    if (hasLiveConversionReading) {
      if (position == 0) {
        rendererCommand_.mutable_ruby_preedit_rectangle()->CopyFrom(
            rendererCommand_.preedit_rectangle());
      } else {
        // A passive suggestion may be anchored partway through the
        // composition. Ruby represents the reading of the entire preedit and
        // therefore needs the geometry of character index zero.
        NSRect rubyPreeditRect = NSZeroRect;
        NSDictionary *rubyClientData =
            [[self client] attributesForCharacterIndex:0
                                  lineHeightRectangle:&rubyPreeditRect];
        if (![self isCurrentNonSecureTextQueryEpoch:geometryEpoch]) {
          [self clearCandidates];
          return;
        }
        [self updateWritingDirectionFromAttributes:rubyClientData];
        const NSPoint rubyBaseline =
            [rubyClientData[@"IMKBaseline"] pointValue];

        SetRendererRectangle(
            rubyPreeditRect, rubyBaseline,
            rendererCommand_.mutable_ruby_preedit_rectangle());
      }
    }

  } @catch (NSException *exception) {
    LOG(ERROR) << "Exception from [" << clientBundle_ << "] " << [[exception name] UTF8String]
               << "," << [[exception reason] UTF8String];
  }

  if (![self isCurrentNonSecureTextQueryEpoch:geometryEpoch]) {
    [self clearCandidates];
    return;
  }
  mozcRenderer_->ExecCommand(rendererCommand_);
}

- (void)updateCandidates:(const Output *)output {
  if (output == nullptr) {
    [self clearCandidates];
    return;
  }

  const bool suppress_candidate_window =
      ShouldSuppressCandidateWindowForLiveConversion(
          *output, useLiveConversion_);
  if (suppress_candidate_window &&
      !output->pre_live_conversion_reading()) {
    [self clearCandidates];
    return;
  }

  rendererCommand_.set_type(RendererCommand::UPDATE);
  *rendererCommand_.mutable_output() = *output;
  if (suppress_candidate_window) {
    // Keep the pre-live reading output, but preserve the existing behavior of
    // suppressing the unfocused ordinary suggestion while live conversion is
    // enabled.
    rendererCommand_.mutable_output()->clear_candidate_window();
  }
  rendererCommandFocusEpoch_ = grimodexFocusEpoch_;
  rendererCallbackToken_ = NewRendererCallbackToken();
  rendererCommand_.mutable_application_info()->set_renderer_callback_token(
      rendererCallbackToken_);

  // Runs delayedUpdateCandidates in the next event loop.
  // This is because some applications like Google Docs with Chrome returns
  // incorrect cursor position if we call attributesForCharacterIndex here.
  [self performSelector:@selector(delayedUpdateCandidates) withObject:nil afterDelay:0];
}

- (void)openLink:(NSURL *)url {
  // Open a link specified by |url|.  Any opening link behavior should
  // call this method because it checks the capability of application.
  // On some application like login window of screensaver, opening
  // link behavior should not happen because it can cause some
  // security issues.
  if (CanOpenLink(clientBundle_)) {
    [[NSWorkspace sharedWorkspace] openURL:url];
  }
}

- (BOOL)fillSurroundingContext:(mozc::commands::Context *)context client:(id<IMKTextInput>)client {
  if ([self refreshSecureInputState]) {
    context->clear_preceding_text();
    context->clear_following_text();
    return false;
  }
  return [self collectSurroundingContext:context client:client];
}

- (BOOL)collectSurroundingContext:(mozc::commands::Context *)context
                           client:(id<IMKTextInput>)client {
  context->clear_preceding_text();
  context->clear_following_text();
  if ([self refreshSecureInputState]) {
    return false;
  }
  const uint64_t textQueryEpoch = grimodexFocusEpoch_;
  if (!grimodexActive_) {
    return false;
  }
  NSInteger totalLength = [client length];
  if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
    context->clear_preceding_text();
    context->clear_following_text();
    return false;
  }
  if (totalLength <= 0 || totalLength == NSNotFound ||
      totalLength > kGenericSurroundingTextClientLengthLimit) {
    return false;
  }
  NSRange selectedRange = [client selectedRange];
  if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
    context->clear_preceding_text();
    context->clear_following_text();
    return false;
  }
  if (selectedRange.location == NSNotFound || selectedRange.length == NSNotFound) {
    return false;
  }
  const NSUInteger unsignedTotalLength = static_cast<NSUInteger>(totalLength);
  if (selectedRange.location > unsignedTotalLength ||
      selectedRange.length > unsignedTotalLength - selectedRange.location) {
    return false;
  }
  NSRange precedingRange = NSMakeRange(0, selectedRange.location);
  if (selectedRange.location > kMaxSurroundingLength) {
    precedingRange =
        NSMakeRange(selectedRange.location - kMaxSurroundingLength, kMaxSurroundingLength);
  }
  NSAttributedString *precedingAttributedString =
      [client attributedSubstringFromRange:precedingRange];
  if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
    context->clear_preceding_text();
    context->clear_following_text();
    return false;
  }
  NSString *precedingString = [precedingAttributedString string];
  if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
    context->clear_preceding_text();
    context->clear_following_text();
    return false;
  }
  if (precedingString) {
    context->set_preceding_text([precedingString UTF8String]);
  }

  const NSUInteger followingStart = NSMaxRange(selectedRange);
  const NSUInteger followingLength =
      std::min(static_cast<NSUInteger>(kMaxSurroundingLength),
               unsignedTotalLength - followingStart);
  if (followingLength > 0) {
    const NSRange followingRange =
        NSMakeRange(followingStart, followingLength);
    if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
      context->clear_preceding_text();
      context->clear_following_text();
      return false;
    }
    NSAttributedString *followingAttributedString =
        [client attributedSubstringFromRange:followingRange];
    if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
      context->clear_preceding_text();
      context->clear_following_text();
      return false;
    }
    NSString *followingString = [followingAttributedString string];
    if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
      context->clear_preceding_text();
      context->clear_following_text();
      return false;
    }
    if (followingString) {
      context->set_following_text([followingString UTF8String]);
    }
  }
  if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
    context->clear_preceding_text();
    context->clear_following_text();
    return false;
  }
  return true;
}

- (BOOL)fillZenzSurroundingContext:(mozc::commands::Context *)context
                            client:(id<IMKTextInput>)client
                   precedingLength:(uint32_t)precedingLength
                   followingLength:(uint32_t)followingLength {
  if (context == nullptr || client == nil) {
    return false;
  }
  if (precedingLength == 0 && followingLength == 0) {
    return true;
  }

  // Zenz context is host text.  Apply the same boundary as the generic
  // collector: never query the client under Secure Event Input or outside the
  // active input domain, and recheck after every potentially reentrant host
  // call so text read for a previous domain is never forwarded.
  const auto abandon = [context]() -> BOOL {
    context->clear_zenz_preceding_text();
    context->clear_zenz_following_text();
    return false;
  };
  if ([self refreshSecureInputState] || !grimodexActive_) {
    return abandon();
  }
  const uint64_t textQueryEpoch = grimodexFocusEpoch_;

  const NSUInteger totalLength = [client length];
  if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
    return abandon();
  }
  if (totalLength == NSNotFound) {
    return false;
  }

  const NSRange selectedRange = [client selectedRange];
  if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
    return abandon();
  }
  if (selectedRange.location == NSNotFound ||
      selectedRange.length == NSNotFound) {
    return false;
  }

  const std::optional<mozc::mac::ZenzContextNativeRanges> ranges =
      mozc::mac::GetZenzContextNativeRanges(
          static_cast<size_t>(totalLength),
          static_cast<size_t>(selectedRange.location),
          static_cast<size_t>(selectedRange.length), precedingLength,
          followingLength);
  if (!ranges.has_value()) {
    return false;
  }

  bool succeeded = true;

  if (precedingLength > 0 && ranges->preceding.has_value()) {
    if (context->has_preceding_text() &&
        mozc::Util::CharsLen(context->preceding_text()) >= precedingLength) {
      context->set_zenz_preceding_text(
          mozc::mac::TakeTrailingZenzContextCharacters(
              context->preceding_text(), precedingLength));
    } else {
      const mozc::mac::ZenzContextNativeRange range = *ranges->preceding;
      if (range.length == 0) {
        context->set_zenz_preceding_text("");
      } else {
        @try {
          NSAttributedString *nativeAttributedText =
              [client attributedSubstringFromRange:NSMakeRange(
                          static_cast<NSUInteger>(range.location),
                          static_cast<NSUInteger>(range.length))];
          if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
            return abandon();
          }
          NSString *nativeText = [nativeAttributedText string];
          if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
            return abandon();
          }
          nativeText = TrimIncompleteZenzSurrogateEdges(nativeText);
          const char *utf8 =
              nativeText == nil ? nullptr : [nativeText UTF8String];
          if (utf8 == nullptr) {
            succeeded = false;
          } else {
            context->set_zenz_preceding_text(
                mozc::mac::TakeTrailingZenzContextCharacters(
                    utf8, precedingLength));
          }
        } @catch (NSException *exception) {
          LOG(ERROR)
              << "Exception while acquiring Zenz preceding context from ["
              << clientBundle_ << "]: " << [[exception reason] UTF8String];
          succeeded = false;
        }
      }
    }
  }

  if (followingLength > 0 && ranges->following.has_value()) {
    const mozc::mac::ZenzContextNativeRange range = *ranges->following;
    if (range.length == 0) {
      context->set_zenz_following_text("");
    } else {
      @try {
        NSAttributedString *nativeAttributedText =
            [client attributedSubstringFromRange:NSMakeRange(
                        static_cast<NSUInteger>(range.location),
                        static_cast<NSUInteger>(range.length))];
        if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
          return abandon();
        }
        NSString *nativeText = [nativeAttributedText string];
        if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
          return abandon();
        }
        nativeText = TrimIncompleteZenzSurrogateEdges(nativeText);
        const char *utf8 =
            nativeText == nil ? nullptr : [nativeText UTF8String];
        if (utf8 == nullptr) {
          succeeded = false;
        } else {
          context->set_zenz_following_text(
              mozc::mac::TakeLeadingZenzContextCharacters(
                  utf8, followingLength));
        }
      } @catch (NSException *exception) {
        LOG(ERROR)
            << "Exception while acquiring Zenz following context from ["
            << clientBundle_ << "]: " << [[exception reason] UTF8String];
        succeeded = false;
      }
    }
  }

  if (![self isCurrentNonSecureTextQueryEpoch:textQueryEpoch]) {
    return abandon();
  }
  return succeeded;
}

- (BOOL)isSecureEventInputEnabledForContext {
  if (secureEventInputStateForTest_ >= 0) {
    return secureEventInputStateForTest_ != 0;
  }
  return IsSecureEventInputEnabled();
}

- (void)applyClientHintsToContext:(mozc::commands::Context *)context {
  if (suppressSuggestion_) {
    // TODO(komatsu, horo): Support Google Omnibox too.
    context->add_experimental_features("google_search_box");
  }
  if (hasWritingDirection_ && verticalWriting_) {
    context->set_vertical_writing(true);
  }
}

- (BOOL)handleEvent:(NSEvent *)event client:(id)sender {
  if (event == nullptr || [event isEqual:[NSNull null]]) {
    return NO;
  }
  if ([event type] == NSEventTypeCursorUpdate) {
    [self refreshSecureInputState];
    const uint64_t cursorUpdateEpoch = grimodexFocusEpoch_;
    const NSRange selectionRange = [self selectionRange];
    if (![self isCurrentGrimodexEpoch:cursorUpdateEpoch]) {
      return NO;
    }
    [[self client] setMarkedText:composedString_
                  selectionRange:selectionRange
                replacementRange:replacementRange_];
    return NO;
  }
  if ([event type] != NSEventTypeKeyDown && [event type] != NSEventTypeFlagsChanged) {
    return NO;
  }

  // Establish the security boundary before any branch below can inspect the
  // host application's selection or surrounding text.
  [self refreshSecureInputState];

  // Handle KANA key and EISU key.  We explicitly handles this here
  // for mode switch because some text area such like iPhoto person
  // name editor does not call setValue:forTag:client: method.
  // see:
  // http://www.google.com/support/forum/p/ime/thread?tid=3aafb74ff71a1a69&hl=ja&fid=3aafb74ff71a1a690004aa3383bc9f5d
  if ([event type] == NSEventTypeKeyDown) {
    NSTimeInterval currentTime = [[NSDate date] timeIntervalSince1970];
    const NSTimeInterval elapsedTime = currentTime - lastKeyDownTime_;
    const bool isDoubleTap =
        ([event keyCode] == lastKeyCode_) && (elapsedTime < kDoubleTapInterval);
    lastKeyDownTime_ = currentTime;
    lastKeyCode_ = [event keyCode];

    // these calling of switchMode: can be duplicated if the
    // application sends the setValue:forTag:client: and handleEvent:
    // at the same key event, but that's okay because switchMode:
    // method does nothing if the new mode is same as the current
    // mode.
    if ([event keyCode] == kVK_JIS_Kana) {
      [self switchMode:mozc::commands::HIRAGANA client:sender];
      [self switchDisplayMode];
      if (isDoubleTap) {
        SessionCommand command;
        command.set_type(SessionCommand::CONVERT_REVERSE);
        [self invokeReconvert:&command client:sender];
      }
    } else if ([event keyCode] == kVK_JIS_Eisu) {
      if (isDoubleTap) {
        SessionCommand command;
        command.set_type(SessionCommand::COMMIT_RAW_TEXT);
        [self dispatchSessionCommand:&command client:sender];
      }
      CompositionMode new_mode =
          ([composedString_ length] == 0) ? mozc::commands::DIRECT : mozc::commands::HALF_ASCII;
      [self switchMode:new_mode client:sender];
      [self switchDisplayMode];
    }
  }

  if ([keyCodeMap_ isModeSwitchingKey:event]) {
    // Special hack for Eisu/Kana keys.  Sometimes those key events
    // come to this method but we should ignore them because some
    // applications like PhotoShop is stuck.
    return YES;
  }

  // Get the Mozc key event
  KeyEvent keyEvent;
  if (![keyCodeMap_ getMozcKeyCodeFromKeyEvent:event toMozcKeyEvent:&keyEvent]) {
    // Modifier flags change (not submitted to the server yet), or
    // unsupported key pressed.
    return NO;
  }

  // If the key event is turn on event, the key event has to be sent
  // to the server anyway.
  if (mode_ == mozc::commands::DIRECT && !mozcClient_->IsDirectModeCommand(keyEvent)) {
    // Yen sign special hack: although the current mode is DIRECT,
    // backslash is sent instead of yen sign for JIS yen key with no
    // modifiers.  This behavior is based on the configuration.
    if ([event keyCode] == kVK_JIS_Yen && [event modifierFlags] == 0 &&
        yenSignCharacter_ == mozc::config::Config::BACKSLASH) {
      [self commitText:"\\" client:sender];
      return YES;
    }
    return NO;
  }

  // Send the key event to the server actually
  Output output;

  if (isprint(keyEvent.key_code())) {
    [originalString_ appendFormat:@"%c", keyEvent.key_code()];
  }

  // The Grimodex client context carries the focus epoch and the Secure Event
  // Input state.  Under Secure Event Input it is marked PASSWORD and no
  // application text is inspected (matching the Windows password-scope privacy
  // boundary); otherwise it performs the historical best-effort generic
  // surrounding-context acquisition.
  mozc::commands::Context context =
      [self buildClientContextForClient:sender includeSurroundingText:YES];
  [self applyClientHintsToContext:&context];
  keyEvent.set_mode(mode_);

  uint64_t sendingFocusEpoch = context.grimodex().focus_epoch();
  const bool secureEventInput = context.grimodex().secure_input();
  const bool canAcquireSurroundingContext =
      [composedString_ length] == 0 &&
      CanSelectedRange(clientBundle_) &&
      CanSurroundingText(clientBundle_);

  // Zenz-specific acquisition is independently bounded, so a legacy generic
  // context failure (for example, a document longer than 1000 UTF-16 units)
  // must not suppress the TEST_SEND_KEY preflight.
  if (!secureEventInput && useZenzContextAcquisition_ &&
      canAcquireSurroundingContext) {
    Output testOutput;
    if (mozcClient_->TestSendKeyWithContext(keyEvent, context, &testOutput) &&
        [self isCurrentNonSecureTextQueryEpoch:sendingFocusEpoch]) {
      const uint32_t precedingLength =
          testOutput.zenz_preceding_text_request_length();
      const uint32_t followingLength =
          testOutput.zenz_following_text_request_length();
      if (precedingLength > 0 || followingLength > 0) {
        [self fillZenzSurroundingContext:&context
                                  client:sender
                         precedingLength:precedingLength
                         followingLength:followingLength];
      }
    }
    // The preflight round trip and the host text queries can reenter the run
    // loop.  If Secure Event Input or the input domain changed meanwhile,
    // discard everything collected for the previous domain and send the key
    // with a context rebuilt from the current state.
    if (![self isCurrentNonSecureTextQueryEpoch:sendingFocusEpoch]) {
      context =
          [self buildClientContextForClient:sender includeSurroundingText:NO];
      [self applyClientHintsToContext:&context];
      sendingFocusEpoch = context.grimodex().focus_epoch();
    }
  }

  if (!mozcClient_->SendKeyWithContext(keyEvent, context, &output)) {
    return NO;
  }

  [self processOutput:&output
                 client:sender
      sendingFocusEpoch:sendingFocusEpoch];
  return output.consumed();
}

#pragma mark ControllerCallback
- (void)sendCommand:(const SessionCommand &)command {
  [self refreshSecureInputState];
  const uint64_t rendererCallbackEpoch = rendererCommandFocusEpoch_;
  // Candidate renderer commands are only meaningful for the output that is
  // currently owned by this active domain.  Epoch transitions clear this
  // marker, so a receiver callback cannot revive an old candidate session.
  if (!grimodexActive_ || rendererCallbackEpoch == 0 ||
      rendererCallbackEpoch != grimodexFocusEpoch_ ||
      rendererCallbackToken_ == 0 || !command.has_renderer_callback_token() ||
      command.renderer_callback_token() != rendererCallbackToken_ ||
      !rendererCommand_.visible() || !rendererCommand_.has_output() ||
      !rendererCommand_.has_application_info() ||
      rendererCommand_.application_info().renderer_callback_token() !=
          rendererCallbackToken_) {
    return;
  }
  Output output;
  id client = [self client];
  const mozc::commands::Context context =
      [self buildClientContextForClient:client includeSurroundingText:NO];
  const uint64_t sendingFocusEpoch = context.grimodex().focus_epoch();
  if (sendingFocusEpoch != rendererCallbackEpoch) {
    return;
  }
  SessionCommand scopedCommand = command;
  scopedCommand.clear_renderer_callback_token();
  if (!mozcClient_->SendCommandWithContext(scopedCommand, context, &output)) {
    return;
  }

  [self processOutput:&output
                 client:client
      sendingFocusEpoch:sendingFocusEpoch];
}

- (void)outputResult:(const mozc::commands::Output &)output {
  // This legacy utility callback carries no controller, focus epoch, or
  // request provenance.  It therefore cannot be distinguished from an output
  // queued for a previous application or pre-secure domain.  Keep the ABI for
  // older tools, but fail closed instead of committing unscoped text.
  (void)output;
  DLOG(WARNING) << "Ignoring unscoped renderer utility output";
}

#pragma mark callbacks
- (IBAction)reconversionClicked:(id)sender {
  SessionCommand command;
  command.set_type(SessionCommand::CONVERT_REVERSE);
  [self invokeReconvert:&command client:[self client]];
}

- (IBAction)configClicked:(id)sender {
  MacProcess::LaunchMozcTool("config_dialog");
}

- (IBAction)dictionaryToolClicked:(id)sender {
  MacProcess::LaunchMozcTool("dictionary_tool");
}

- (IBAction)registerWordClicked:(id)sender {
  [self launchWordRegisterTool:[self client]];
}

- (IBAction)aboutDialogClicked:(id)sender {
  MacProcess::LaunchMozcTool("about_dialog");
}

+ (void)setGlobalRendererReceiver:(RendererReceiver *)rendererReceiver {
  gRendererReceiver = rendererReceiver;
}
@end

// An alias of MozcImkInputController for backward compatibility.
@implementation GoogleJapaneseInputController
@end

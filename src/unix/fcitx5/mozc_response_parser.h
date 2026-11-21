// Copyright 2010-2012, Google Inc.
// Copyright 2012~2013, Weng Xuetian <wengxt@gmail.com>
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

#ifndef MOZC_UNIX_FCITX_MOZC_RESPONSE_PARSER_H_
#define MOZC_UNIX_FCITX_MOZC_RESPONSE_PARSER_H_

#include <cstdint>

#include "protocol/candidate_window.pb.h"
#include "protocol/commands.pb.h"
#include "unix/fcitx5/session_callback.h"

namespace fcitx {

class InputContext;
class MozcEngine;

// This class parses IPC response from mozc_server (mozc::commands::Output) and
// updates the FCITX UI.
class MozcResponseParser {
 public:
  MozcResponseParser(MozcEngine* engine);
  MozcResponseParser(const MozcResponseParser&) = delete;
  ~MozcResponseParser();

  // Parses a response from Mozc server and sets persed information on
  // Mozkey Fcitx object. Returns true if response.consumed() is true. The
  // engine must be non-null. This function does not take ownership of it.
  //
  // A delayed callback in the response is scheduled whether or not the
  // response is consumed.  The RECONVERT_SELECTION_OR_INSERT_SPACE callback is
  // not handled here: MozcState resolves it before calling the parser.
  bool ParseResponse(const mozc::commands::Output& response,
                     InputContext* ic) const;

 private:
  // `allow_immediate_callback` is false for the reply to an immediate
  // callback (UNDO, CONVERT_REVERSE), so that callbacks never recurse.
  bool ParseResponseInternal(const mozc::commands::Output& response,
                             InputContext* ic,
                             bool allow_immediate_callback) const;
  void UpdateDeletionRange(const mozc::commands::Output& response,
                           InputContext* ic) const;
  void LaunchTool(const mozc::commands::Output& response,
                  InputContext* ic) const;
  // Runs the callbacks that are executed at once: kUndo and kConvertReverse.
  void ExecuteCallback(CallbackAction action, InputContext* ic) const;
  void ParseResult(const mozc::commands::Result& result,
                   InputContext* ic) const;
  void ParseCandidates(const mozc::commands::CandidateWindow& candidates,
                       InputContext* ic) const;
  void ParsePreedit(const mozc::commands::Preedit& preedit, uint32_t position,
                    InputContext* ic) const;

  MozcEngine* engine_;
};

}  // namespace fcitx

#endif  // MOZC_UNIX_FCITX_MOZC_RESPONSE_PARSER_H_

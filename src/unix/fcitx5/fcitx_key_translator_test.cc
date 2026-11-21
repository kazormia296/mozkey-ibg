// Copyright 2026 The Mozkey Authors
// Licensed under the Apache License, Version 2.0.

#include "unix/fcitx5/fcitx_key_translator.h"

#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>

#include <cstdint>

#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "testing/gunit.h"

namespace fcitx {
namespace {

using mozc::commands::KeyEvent;
using mozc::config::Config;

bool Translate(KeySym keyval, KeyStates modifiers, KeyEvent* key) {
  const KeyTranslator translator;
  return translator.Translate(keyval, /*keycode=*/0, modifiers, Config::ROMAN,
                              /*layout_is_jp=*/true, key);
}

bool HasModifier(const KeyEvent& key, KeyEvent::ModifierKey modifier) {
  for (int i = 0; i < key.modifier_keys_size(); ++i) {
    if (key.modifier_keys(i) == modifier) {
      return true;
    }
  }
  return false;
}

TEST(FcitxKeyTranslatorTest, PlainAsciiKey) {
  KeyEvent key;
  ASSERT_TRUE(Translate(FcitxKey_a, KeyState::NoState, &key));
  EXPECT_EQ(key.key_code(), static_cast<uint32_t>('a'));
  EXPECT_EQ(key.modifier_keys_size(), 0);
}

// Super + key belongs to Fcitx and the desktop (Super+Space, ...).
TEST(FcitxKeyTranslatorTest, SuperIsNotHandled) {
  KeyEvent key;
  EXPECT_FALSE(Translate(FcitxKey_space, KeyState::Super, &key));
  EXPECT_FALSE(Translate(FcitxKey_a, KeyState::Super, &key));
}

// GTK based frontends report virtual Super / Hyper bits; they are refused
// with or without the real Mod4 bit.
TEST(FcitxKeyTranslatorTest, VirtualSuperAndHyperAreNotHandled) {
  KeyEvent key;
  EXPECT_FALSE(Translate(FcitxKey_space, KeyState::Super2, &key));
  EXPECT_FALSE(Translate(FcitxKey_space, KeyState::Hyper2, &key));
  EXPECT_FALSE(Translate(
      FcitxKey_a,
      KeyStates{KeyState::Super, KeyState::Super2, KeyState::Hyper2}, &key));
}

// https://github.com/google/mozc/issues/1590: Mod3 (Level 5 Shift, Neo2 /
// Bone layouts) and Mod5 (Level 3 Shift, AltGr) only pick the character.
TEST(FcitxKeyTranslatorTest, LevelShiftModifiersTypeCharacters) {
  KeyEvent key;
  ASSERT_TRUE(Translate(FcitxKey_minus, KeyState::Mod3, &key));
  EXPECT_EQ(key.key_code(), static_cast<uint32_t>('-'));
  EXPECT_EQ(key.modifier_keys_size(), 0);

  ASSERT_TRUE(Translate(FcitxKey_at, KeyState::Mod5, &key));
  EXPECT_EQ(key.key_code(), static_cast<uint32_t>('@'));
  EXPECT_EQ(key.modifier_keys_size(), 0);
}

TEST(FcitxKeyTranslatorTest, NumLockDoesNotAffectKeys) {
  KeyEvent key;
  ASSERT_TRUE(Translate(FcitxKey_a, KeyState::NumLock, &key));
  EXPECT_EQ(key.key_code(), static_cast<uint32_t>('a'));
  EXPECT_EQ(key.modifier_keys_size(), 0);
}

// Meta usually shares Mod1 with Alt; Alt shortcuts must keep working when a
// frontend reports both.
TEST(FcitxKeyTranslatorTest, AltShortcutSurvivesMetaBit) {
  KeyEvent key;
  ASSERT_TRUE(
      Translate(FcitxKey_a, KeyStates{KeyState::Alt, KeyState::Meta}, &key));
  EXPECT_EQ(key.key_code(), static_cast<uint32_t>('a'));
  EXPECT_TRUE(HasModifier(key, KeyEvent::ALT));
}

TEST(FcitxKeyTranslatorTest, CtrlAndAltAreReported) {
  KeyEvent key;
  ASSERT_TRUE(
      Translate(FcitxKey_a, KeyStates{KeyState::Ctrl, KeyState::Alt}, &key));
  EXPECT_TRUE(HasModifier(key, KeyEvent::CTRL));
  EXPECT_TRUE(HasModifier(key, KeyEvent::ALT));
}

}  // namespace
}  // namespace fcitx
